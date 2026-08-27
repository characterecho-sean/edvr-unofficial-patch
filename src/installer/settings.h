// Every EDVR setting, as data the window can lay out.
//
// The table itself is generated (tools/gen_settings_schema.py) from the two
// places the facts already live: the accessor call in the code gives the type,
// the range and the default, and the comment block above the key in edvr.ini
// gives the explanation. Writing that down a third time in C++ would be a
// fourth list to forget to update -- which is the failure
// tools/check_config_contract.py exists to catch between the other two.
//
// The only thing added by hand is what neither source can know: what to call a
// setting in a list, and which value is RECOMMENDED. Those live on one
// annotation line above the key in edvr.ini, and the generator refuses to build
// if a setting that is live in that file does not have one -- a fix promoted to
// shipped-on has to be reachable by somebody who does not edit ini files.
#pragma once

#include <string>
#include <vector>

namespace edvr::installer {

enum class SettingKind { Toggle, Number, Choice, Text };

struct SettingDef {
    const char* section;
    const char* key;
    const char* label;
    const char* summary;      // one sentence, for the row
    const char* description;  // the whole comment block from edvr.ini
    SettingKind kind;
    const char* shipped;      // the value edvr.ini ships with
    const char* recommended;  // usually the same; sometimes a tested pairing
    const char* lo;           // range, when the accessor declares one
    const char* hi;
    int         precision;    // decimals to show for a number
    const char* choices;      // "vivid|realistic|stock", empty for other kinds
    bool        live;         // shipped uncommented
};

struct Choice {
    std::string value;  // what goes into the file
    std::string label;  // what the window shows, when they differ
};

struct SettingRow {
    const SettingDef*   def = nullptr;
    std::string         value;  // what the user's edvr.ini says now
    std::vector<Choice> choices;
    bool                isRecommended = false;
    bool                isShipped = false;

    // "default 45", plus the bounds when they are known. Empty for a toggle:
    // on and off need no explaining, and a row of them repeating "default on"
    // is noise.
    std::wstring helper() const;
};

// Every setting, in the order the ini defines them, grouped by section.
const std::vector<SettingDef>& settingDefs();

// The settings of one install, read from its edvr.ini and written back to it.
class SettingsModel {
public:
    // Reads <gameDir>\edvr.ini. Absent is not an error: every setting then
    // reads at its shipped value, and the first change writes the file.
    bool load(const std::wstring& gameDir);

    bool loaded() const { return m_loaded; }
    const std::wstring& iniPath() const { return m_iniPath; }
    const std::vector<SettingRow>& rows() const { return m_rows; }

    // Writes the value into edvr.ini, keeping the file's layout, comments and
    // every other value exactly as they were. The game re-reads the file within
    // about a second, so this IS the apply step.
    bool set(size_t index, const std::string& value);

    const std::string& lastError() const { return m_error; }

private:
    void refreshRows();

    bool                   m_loaded = false;
    std::wstring           m_iniPath;
    std::string            m_text;   // the file as it stands
    std::vector<SettingRow> m_rows;
    std::string            m_error;
};

// Split "a|b|c", each optionally "value=label".
std::vector<Choice> splitChoices(const char* packed);

}  // namespace edvr::installer
