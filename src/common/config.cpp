#include "config.h"

#include <windows.h>

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "log.h"

namespace edvr {

struct Config::Impl {
    std::map<std::string, std::string> values;
};

Config& Config::get() {
    static Config instance;
    return instance;
}

std::wstring moduleDirectory(void* hModule) {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(static_cast<HMODULE>(hModule), buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    std::wstring s(buf, n);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : s.substr(0, slash);
}

std::wstring executableDirectory() { return moduleDirectory(nullptr); }

bool ensureDirectory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

void Config::init(const std::wstring& moduleDir) {
    if (!m_impl) m_impl = new Impl();

    const std::wstring candidates[] = {
        moduleDir + L"\\edvr.ini",
        executableDirectory() + L"\\edvr.ini",
    };
    m_path = candidates[0];
    for (const std::wstring& c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
            m_path = c;
            break;
        }
    }
    parse();

    // Defaults to <exe dir>\edvr_logs; the shader dump lands in a subdirectory.
    std::string dirUtf8 = getString("log.dir", "");
    if (dirUtf8.empty()) {
        m_logDir = executableDirectory() + L"\\edvr_logs";
    } else {
        const int need = MultiByteToWideChar(CP_UTF8, 0, dirUtf8.c_str(), -1, nullptr, 0);
        std::vector<wchar_t> w(need > 0 ? need : 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, dirUtf8.c_str(), -1, w.data(), need);
        m_logDir = w.data();
    }
    // Not created here. Log::open() makes it when it actually opens a file, so
    // logging turned off leaves no empty directory behind.
}

void Config::parse() {
    // Parsed into a local and swapped in only on success.
    //
    // This used to clear every value first and then open the file. A failed open
    // -- an editor holding the file, or the momentary absence while a save-by-
    // rename completes -- therefore dropped the whole configuration to defaults,
    // and the reload poll runs about once a second with a text editor open,
    // which is exactly when somebody is editing. reloadIfChanged() would report
    // success too. Nothing survives being read at the wrong instant now.
    std::map<std::string, std::string> parsed;

    HANDLE f = CreateFileW(m_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        // An ini that is genuinely absent means "all defaults", which is only
        // true on the FIRST parse; later it means the file went away mid-session
        // and the values we already have are better than nothing.
        if (m_impl->values.empty()) m_impl->values.swap(parsed);
        return;
    }

    const DWORD size = GetFileSize(f, nullptr);
    if (size == INVALID_FILE_SIZE || size > (1u << 20)) {
        // Deliberately WITHOUT stamping m_lastWrite. Stamping first meant a file
        // that was briefly unreadable -- or briefly enormous -- was never read
        // again if it came back with the same timestamp, which is what restoring
        // a backup or a git checkout does.
        CloseHandle(f);
        return;
    }

    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL haveInfo = GetFileInformationByHandle(f, &info);
    std::vector<char> text(size + 1, 0);
    DWORD read = 0;
    const BOOL readOk = ReadFile(f, text.data(), size, &read, nullptr);
    CloseHandle(f);
    // A failed or short read is not an empty file. The result was discarded, so
    // `read` came back 0, the parse produced nothing, and the swap below
    // installed an empty map -- every setting reverted to its compiled-in
    // default for the rest of the session, because m_lastWrite had already been
    // stamped. Exactly what the swap-on-success was added to prevent.
    if (!readOk || read < size) return;

    const char* p = text.data();
    const char* end = p + read;

    // Skip a UTF-8 byte order mark.
    //
    // Notepad writes one by default, so an edvr.ini a user edited and saved is
    // likely to start with EF BB BF. Without this the BOM sticks to the front of
    // the first line, "[fix]" stops looking like a section header, and every
    // setting under it is filed under the WRONG section -- the file loads
    // "successfully", the log looks clean, and nothing the user changed has any
    // effect. Exactly the bug report nobody can diagnose.
    if (read >= 3 && static_cast<unsigned char>(p[0]) == 0xEF &&
        static_cast<unsigned char>(p[1]) == 0xBB &&
        static_cast<unsigned char>(p[2]) == 0xBF) {
        p += 3;
    }

    std::string section;
    while (p < end) {
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') ++lineEnd;
        std::string line(p, lineEnd);
        p = lineEnd;
        while (p < end && (*p == '\n' || *p == '\r')) ++p;

        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        line = line.substr(first);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            const size_t close = line.find(']');
            if (close != std::string::npos) section = line.substr(1, close - 1);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // A trailing comment ends the value.
        //
        // Whole comment LINES were skipped above, but a comment after a value
        // was kept as part of it: "black_void = 1 ; keep this on" read as the
        // string "1 ; keep this on", which is not "1", so the fix the user was
        // annotating to keep switched off. Every ini in the world lets you do
        // this and nothing warned.
        //
        // Only when the marker follows whitespace, so a value that legitimately
        // contains one -- a filename with a '#' -- survives.
        for (size_t i = 1; i < val.size(); ++i) {
            if ((val[i] == ';' || val[i] == '#') && (val[i - 1] == ' ' || val[i - 1] == '\t')) {
                val.erase(i);
                break;
            }
        }

        auto trim = [](std::string& s) {
            const size_t a = s.find_first_not_of(" \t");
            if (a == std::string::npos) { s.clear(); return; }
            const size_t b = s.find_last_not_of(" \t");
            s = s.substr(a, b - a + 1);
        };
        trim(key);
        trim(val);
        if (key.empty()) continue;
        // Section-qualified keys, so [openvr] hook_compositor reads as
        // "openvr.hook_compositor". Bare "a.b = c" also works.
        parsed[section.empty() ? key : section + "." + key] = val;
    }
    m_impl->values.swap(parsed);
    // Stamped only now, on the success path.
    //
    // Stamping it up front meant a read that failed -- an editor holding the
    // file mid-save -- kept the old values, correctly, but recorded the new
    // timestamp with them. reloadIfChanged() then saw nothing to do and that
    // edit was never picked up for the rest of the session. The size-check bail
    // above already avoided this; the read path did not.
    if (haveInfo) m_lastWrite = info.ftLastWriteTime;
}

bool Config::reloadIfChanged() {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(m_path.c_str(), GetFileExInfoStandard, &data)) return false;
    if (CompareFileTime(&data.ftLastWriteTime, &m_lastWrite) == 0) return false;
    parse();
    return true;
}

std::string Config::getString(const char* key, const char* def) const {
    if (!m_impl) return def ? def : "";
    auto it = m_impl->values.find(key);
    if (it != m_impl->values.end()) return it->second;
    return std::string(def ? def : "");
}

bool Config::getBool(const char* key, bool def) const {
    std::string v = getString(key, "");
    if (v.empty()) return def;
    for (char& c : v) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;

    // Unrecognised, so the DEFAULT -- not false.
    //
    // The old list was six exact spellings with no case folding, so "On", "YES"
    // and "True " all fell through to false. For a setting that defaults to true
    // that means typing a word meaning "yes" switched the fix OFF, silently,
    // which is the worst possible reading of the user's intent.
    //
    // One key cannot be reported this way: log.enabled is read before the log is
    // open, and note() no-ops until then. Everything else lands.
    Log::get().note("edvr.ini: %s = \"%s\" is not a yes/no value, so the default (%s) is "
                    "being used. Write 1/0, true/false, yes/no or on/off.",
                    key, v.c_str(), def ? "on" : "off");
    return def;
}

int Config::getInt(const char* key, int def) const {
    const std::string v = getString(key, "");
    if (v.empty()) return def;
    return static_cast<int>(strtol(v.c_str(), nullptr, 0));
}

float Config::getFloat(const char* key, float def) const {
    const std::string v = getString(key, "");
    if (v.empty()) return def;
    return strtof(v.c_str(), nullptr);
}

}  // namespace edvr
