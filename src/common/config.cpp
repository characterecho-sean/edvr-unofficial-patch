#include "config.h"

#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "log.h"

namespace edvr {

struct Config::Impl {
    std::map<std::string, std::string> values;
    // The config audit's session memory: findings queued until the log can
    // take them (the first parse runs before Log::open), and a set of what
    // has already been said so a reload does not repeat it.
    std::vector<std::string> auditPending;
    std::set<std::string>    auditNoted;
};

namespace {
// Registered by config_audit.cpp (DLLs) or by a test; null means no audit.
const char* const*        g_auditKnown = nullptr;
size_t                    g_auditKnownCount = 0;
const char* const (*g_auditMoved)[2] = nullptr;
size_t                    g_auditMovedCount = 0;

std::string lowered(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}
}  // namespace

void Config::setAuditTables(const char* const* knownLower, size_t knownCount,
                            const char* const (*movedOldNew)[2],
                            size_t movedCount) {
    g_auditKnown = knownLower;
    g_auditKnownCount = knownCount;
    g_auditMoved = movedOldNew;
    g_auditMovedCount = movedCount;
}

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
        // "openvr.hook_compositor". Bare "a.b = c" also works. Lowercased,
        // matching the installer's merge: every key this build reads is
        // lowercase, so a hand-typed "FSS_Res = 1" used to be filed under a
        // spelling nothing looks up -- a line that does nothing and looks
        // right.
        parsed[lowered(section.empty() ? key : section + "." + key)] = val;
    }
    auditResolve(&parsed);
    m_impl->values.swap(parsed);
    // Stamped only now, on the success path.
    //
    // Stamping it up front meant a read that failed -- an editor holding the
    // file mid-save -- kept the old values, correctly, but recorded the new
    // timestamp with them. reloadIfChanged() then saw nothing to do and that
    // edit was never picked up for the rest of the session. The size-check bail
    // above already avoided this; the read path did not.
    if (haveInfo) m_lastWrite = info.ftLastWriteTime;
    auditFlush();
}

// The parsed file against the registered tables: moved keys resolved, dead
// lines named. Findings are queued -- the first parse runs before the log
// opens -- and each is said once per session, so a live reload does not
// repeat them.
void Config::auditResolve(void* parsedMap) {
    if (!g_auditKnown || !m_impl) return;
    auto& parsed = *static_cast<std::map<std::string, std::string>*>(parsedMap);

    std::set<std::string> movedOld;
    std::set<std::string> synthesized;
    for (size_t i = 0; i < g_auditMovedCount; ++i) {
        const std::string oldK = g_auditMoved[i][0];
        const std::string newK = g_auditMoved[i][1];
        movedOld.insert(oldK);
        auto o = parsed.find(oldK);
        if (o == parsed.end()) continue;
        auto n = parsed.find(newK);
        if (n == parsed.end()) {
            // The old-layout line still works: its value is read as the new
            // key this session, and the log says how to make that permanent.
            parsed[newK] = o->second;
            synthesized.insert(newK);
            if (m_impl->auditNoted.insert("mv:" + oldK).second) {
                m_impl->auditPending.push_back(
                    "edvr.ini: " + oldK + " has moved to " + newK +
                    " -- your value (" + o->second + ") is being read from "
                    "the old line this session. The installer's update "
                    "migrates the file, or move the line yourself.");
            }
        } else if (n->second != o->second && !synthesized.count(newK)) {
            // Two settings can merge into one new key; a second old value
            // arriving after the first synthesized the target is not the
            // user contradicting themselves, so the conflict note is only
            // for a target genuinely set in the file.
            if (m_impl->auditNoted.insert("mv2:" + oldK).second) {
                m_impl->auditPending.push_back(
                    "edvr.ini: both " + oldK + " and " + newK + " are set, "
                    "with different values. The new name wins (" + n->second +
                    "); delete the old line.");
            }
        }
    }

    std::set<std::string> known;
    for (size_t i = 0; i < g_auditKnownCount; ++i) known.insert(g_auditKnown[i]);
    std::string dead;
    int deadCount = 0;
    for (const auto& kv : parsed) {
        if (known.count(kv.first) || movedOld.count(kv.first)) continue;
        if (!m_impl->auditNoted.insert("uk:" + kv.first).second) continue;
        if (!dead.empty()) dead += ", ";
        if (deadCount < 8) dead += kv.first;
        ++deadCount;
    }
    if (deadCount) {
        if (deadCount > 8) dead += ", ...";
        m_impl->auditPending.push_back(
            "edvr.ini: " + std::to_string(deadCount) +
            " line(s) name settings this build does not read: " + dead +
            ". A typo or a retired setting -- those lines do nothing.");
    }
}

void Config::auditFlush() const {
    if (!m_impl || m_impl->auditPending.empty() || !Log::get().isOpen()) return;
    for (const std::string& s : m_impl->auditPending) {
        Log::get().note("%s", s.c_str());
    }
    m_impl->auditPending.clear();
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
    // The audit's findings wait here for the log: the first parse runs before
    // Log::open, and the first config read after it opens is soon enough.
    auditFlush();
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

void Config::set(const char* key, const char* value) {
    if (!key || !value) return;
    if (!m_impl) m_impl = new Impl();
    std::string k(key);
    for (char& c : k) c = static_cast<char>(tolower(c));
    m_impl->values[k] = value;
}

// Does the whole value parse, or only a prefix of it?
//
// strtol stops at the first character it cannot use and, with a null end
// pointer, says nothing about it. So "3w" reads as 3 and "2 or 3" reads as 2.
// Found in the field: a stray keystroke had left
// transition_flash_max_consecutive = 3w in a player's ini, which parsed to 3
// -- and 3 happens to equal the burst budget, so every excursion spent the
// whole budget and opened a two-second window where nothing could be
// withheld. The typo was invisible; the flash was not.
//
// Trailing whitespace is fine. Anything else means the line does not say
// what its author thought it said, and the default is the safer reading.
static bool wholeValueParsed(const char* s, const char* end) {
    if (end == s) return false;
    while (*end == ' ' || *end == 0x09) ++end;
    return *end == 0;
}

int Config::getInt(const char* key, int def) const {
    const std::string v = getString(key, "");
    if (v.empty()) return def;
    const char* s = v.c_str();
    char* end = nullptr;
    const long raw = strtol(s, &end, 0);
    if (!wholeValueParsed(s, end)) {
        Log::get().note("%s = \"%s\" is not a plain number; using %d. Everything "
                        "after the digits was ignored before this, which made a "
                        "typo read as a deliberate setting.", key, s, def);
        return def;
    }
    return static_cast<int>(raw);
}

float Config::getFloat(const char* key, float def) const {
    const std::string v = getString(key, "");
    if (v.empty()) return def;
    // A value that does not parse reads 0.0 silently, and 0.0 is a legitimate
    // setting for most of these -- so "I typed 2,75 in a comma locale" and "I
    // meant 0" are indistinguishable in the log and in the headset. strtof's
    // endptr tells them apart, so it is used.
    const char* s = v.c_str();
    char* end = nullptr;
    const float out = strtof(s, &end);
    if (end == s) {
        Log::get().note("%s = \"%s\" is not a number; using %g. If you meant a "
                        "decimal, use a point rather than a comma.", key, s, def);
        return def;
    }
    return out;
}

// Bounded integers, because every unbounded one has cost something.
//
// getInt returns whatever strtol produces and the caller casts it. Cast to
// uint32_t, -1 becomes 4294967295: an intent grace period of thirteen hours, an
// entry window that never closes, a plausibility filter that admits every
// value. Four separate settings reached that state and none of them said a
// word -- the failure is always "the feature behaves as though the setting were
// absent", which is the hardest kind to attribute.
//
// Clamping rather than refusing, and SAYING SO, for the same reason the head
// offset clamps: a refused value silently becomes a default that is nothing
// like what was asked for, where a clamped one is the nearest thing that works.
int Config::getIntInRange(const char* key, int def, int lo, int hi) const {
    const std::string v = getString(key, "");
    if (v.empty()) return def;
    const char* s = v.c_str();
    char* end = nullptr;
    const long raw = strtol(s, &end, 0);
    if (!wholeValueParsed(s, end)) {
        Log::get().note("%s = \"%s\" is not a number; using %d.", key, s, def);
        return def;
    }
    if (raw < lo || raw > hi) {
        const long c = raw < lo ? lo : hi;
        Log::get().note("%s = %ld is outside %d..%d, so %ld is being used.",
                        key, raw, lo, hi, c);
        return static_cast<int>(c);
    }
    return static_cast<int>(raw);
}

}  // namespace edvr
