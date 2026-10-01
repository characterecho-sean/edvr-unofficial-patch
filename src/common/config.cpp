#include "config.h"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "log.h"
#include "runtime_profile.h"

namespace edvr {

// std::less<> makes the map transparent, so find(const char*) compares in
// place. With the default comparator every getter built a temporary
// std::string just to look its key up, and most keys are longer than the 15
// characters a std::string stores inline, so that was a heap allocation per
// read.
using ValueMap = std::map<std::string, std::string, std::less<>>;

struct Config::Impl {
    ValueMap values;
    // The config audit's session memory: findings queued until the log can
    // take them (the first parse runs before Log::open), and a set of what
    // has already been said so a reload does not repeat it.
    std::vector<std::string> auditPending;
    std::set<std::string>    auditNoted;
    // Keys whose malformed-value note has been written since the last
    // successful parse. A parse clears it, so an edit that leaves a value
    // malformed says so again -- once. See firstNoteFor().
    std::set<std::string, std::less<>> noteNoted;
};

namespace {
// Registered by config_audit.cpp (DLLs) or by a test; null means no audit.
const char* const*        g_auditKnown = nullptr;
size_t                    g_auditKnownCount = 0;
const char* const (*g_auditMoved)[3] = nullptr;
size_t                    g_auditMovedCount = 0;

std::string lowered(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Config's lock, scoped. Shared for a lookup; exclusive for anything that
// writes what m_impl points at. See the comment on Config::m_lock for what
// may and may not happen while one of these is alive.
class ReadGuard {
public:
    explicit ReadGuard(SRWLOCK& lock) : m_lock(lock) { AcquireSRWLockShared(&m_lock); }
    ~ReadGuard() { ReleaseSRWLockShared(&m_lock); }
    ReadGuard(const ReadGuard&) = delete;
    ReadGuard& operator=(const ReadGuard&) = delete;
private:
    SRWLOCK& m_lock;
};

class WriteGuard {
public:
    explicit WriteGuard(SRWLOCK& lock) : m_lock(lock) { AcquireSRWLockExclusive(&m_lock); }
    ~WriteGuard() { ReleaseSRWLockExclusive(&m_lock); }
    WriteGuard(const WriteGuard&) = delete;
    WriteGuard& operator=(const WriteGuard&) = delete;
private:
    SRWLOCK& m_lock;
};
}  // namespace

void Config::setAuditTables(const char* const* knownLower, size_t knownCount,
                            const char* const (*movedOldNew)[3],
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

namespace {
std::wstring environmentValue(const wchar_t* name) {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

// Where logs go when the ini does not say: <exe dir>\edvr_logs, unless the
// environment moves it. EDVR_LOG_DIR names the directory; EDVR_LOG_DIR_FOR,
// when set, limits the move to processes whose exe sits in that directory.
// build.bat's test runner sets both, so every proxy a rig loads from build\
// logs -- and arms its crash sentinels -- in a directory of the rig's own,
// while the children some rigs stage in private directories, and read the
// logs of, keep their exe-relative default.
std::wstring defaultLogDirectory() {
    const std::wstring exeDir = executableDirectory();
    const std::wstring moved = environmentValue(L"EDVR_LOG_DIR");
    if (moved.empty()) return exeDir + L"\\edvr_logs";
    const std::wstring only = environmentValue(L"EDVR_LOG_DIR_FOR");
    if (!only.empty() && _wcsicmp(only.c_str(), exeDir.c_str()) != 0) return exeDir + L"\\edvr_logs";
    return moved;
}
}  // namespace

void Config::init(const std::wstring& moduleDir) {
    runtimeProfileInitialize(moduleDir, executableDirectory());
    {
        // Under the lock, because a getter on another thread reads the pointer
        // itself: it is null until here, and "no Impl yet" means "the default".
        WriteGuard g(m_lock);
        if (!m_impl) m_impl = new Impl();
    }

    // The flat profile keeps its settings in a file of its own, so a flat
    // install over a VR one (or back) never clobbers the other profile's
    // tuning. A flat install without edvr-flat.ini yet falls back to
    // edvr.ini, which is how existing flat installs keep their settings
    // until the installer seeds the separate file.
    //
    // One file is read, whole: m_path is the first candidate that exists, and iniName()
    // (config.h) names it for every message that has to say which file it means.
    std::wstring candidates[4];
    size_t count = 0;
    if (runtimeFlatProfile()) {
        candidates[count++] = moduleDir + L"\\" + kIniNameFlatW;
        candidates[count++] = executableDirectory() + L"\\" + kIniNameFlatW;
    }
    candidates[count++] = moduleDir + L"\\" + kIniNameVrW;
    candidates[count++] = executableDirectory() + L"\\" + kIniNameVrW;
    m_path = candidates[0];
    for (size_t i = 0; i < count; ++i) {
        if (GetFileAttributesW(candidates[i].c_str()) != INVALID_FILE_ATTRIBUTES) {
            m_path = candidates[i];
            break;
        }
    }
    parse();

    // Defaults to <exe dir>\edvr_logs; the shader dump lands in a subdirectory.
    std::string dirUtf8 = getString("log.dir", "");
    if (dirUtf8.empty()) {
        m_logDir = defaultLogDirectory();
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
    //
    // Nor is the lock held while the file is read and parsed. That is the slow
    // part -- a 148 KB ini -- and it touches nothing another thread can see.
    // The lock is taken once, below, to swap the result in.
    ValueMap parsed;

    // FILE_SHARE_DELETE, so that reading the ini does not stop it being replaced,
    // renamed or deleted. The in-headset menu and the installer's settings window
    // both save through iniedit's writeFileAtomic, which renames a temp file over
    // edvr.ini, and the menu asks for a reload straight after -- so the next save
    // can land while this read has the file open.
    //
    // That rename is done with POSIX semantics (FileRenameInfoEx, Windows 10 1607+,
    // NTFS), which goes through under a handle that shares DELETE: the target's
    // name moves to the new file at once and this read carries on with the old
    // one. Without the share it would be refused for as long as this read lasts.
    // (The classic MoveFileExW replace, which writeFileAtomic falls back to where
    // POSIX renames are unsupported, is refused while ANY handle to the target is
    // open, this share included -- measured on Windows 11 build 26200 -- and there
    // it is the writer's retry that waits this read out. See iniedit.h.)
    HANDLE f = CreateFileW(m_path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        // An ini that is genuinely absent means "all defaults", which is only
        // true on the FIRST parse; later it means the file went away mid-session
        // and the values we already have are better than nothing.
        //
        // `parsed` is still empty here, so this can only leave an empty map
        // empty. It is a write to shared state all the same, and is locked as
        // one.
        WriteGuard g(m_lock);
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
    {
        // The only stretch of parse() under the lock: the audit's bookkeeping,
        // the swap, the note set's reset and the stamp. Nothing in here calls
        // a getter or the log, so it cannot meet the lock again.
        WriteGuard g(m_lock);
        auditResolve(&parsed);
        m_impl->values.swap(parsed);
        m_impl->noteNoted.clear();
        // Stamped only now, on the success path.
        //
        // Stamping it up front meant a read that failed -- an editor holding
        // the file mid-save -- kept the old values, correctly, but recorded
        // the new timestamp with them. reloadIfChanged() then saw nothing to
        // do and that edit was never picked up for the rest of the session.
        // The size-check bail above already avoided this; the read path did
        // not.
        if (haveInfo) m_lastWrite = info.ftLastWriteTime;
    }
    // `parsed` now owns the PREVIOUS map. It is freed when this function
    // returns, after the lock is gone, so a reader is never held up by the
    // destructor of a map it is not using.
    auditFlush();
}

// The parsed file against the registered tables: moved keys resolved, dead
// lines named. Findings are queued -- the first parse runs before the log
// opens -- and each is said once per session, so a live reload does not
// repeat them.
//
// Runs under the exclusive lock (parse() takes it): it reads and writes
// Impl's audit sets. It only queues; auditFlush() does the logging, unlocked.
void Config::auditResolve(void* parsedMap) {
    if (!g_auditKnown || !m_impl) return;
    auto& parsed = *static_cast<ValueMap*>(parsedMap);

    // The file every finding below is about: the one this parse read.
    const std::string file = iniName();
    std::set<std::string> movedOld;
    std::set<std::string> synthesized;
    for (size_t i = 0; i < g_auditMovedCount; ++i) {
        const std::string oldK = g_auditMoved[i][0];
        const std::string newK = g_auditMoved[i][1];
        movedOld.insert(oldK);
        auto o = parsed.find(oldK);
        if (o == parsed.end()) continue;
        const std::string oldDefault = g_auditMoved[i][2];
        const bool staleDefault = !oldDefault.empty() && o->second == oldDefault;
        auto n = parsed.find(newK);
        if (n == parsed.end()) {
            if (staleDefault) {
                // The line carries the OLD key's shipped default -- nobody's
                // choice, just an un-updated file. The new key's own default
                // (which may have flipped) must rule, so nothing is
                // synthesized from it.
                if (m_impl->auditNoted.insert("mv:" + oldK).second) {
                    m_impl->auditPending.push_back(
                        file + ": " + oldK + " has moved to " + newK +
                        ", and your line still carries the retired default (" +
                        o->second + "), so it is ignored and " + newK +
                        "'s own default applies. The installer's update "
                        "tidies the file.");
                }
                continue;
            }
            // The old-layout line still works: its value is read as the new
            // key this session, and the log says how to make that permanent.
            parsed[newK] = o->second;
            synthesized.insert(newK);
            if (m_impl->auditNoted.insert("mv:" + oldK).second) {
                m_impl->auditPending.push_back(
                    file + ": " + oldK + " has moved to " + newK +
                    " -- your value (" + o->second + ") is being read from "
                    "the old line this session. The installer's update "
                    "migrates the file, or move the line yourself.");
            }
        } else if (staleDefault) {
            // Both set, but the old line is only the retired default: the
            // new line rules and there is nothing worth a warning.
        } else if (n->second != o->second && !synthesized.count(newK)) {
            // Two settings can merge into one new key; a second old value
            // arriving after the first synthesized the target is not the
            // user contradicting themselves, so the conflict note is only
            // for a target genuinely set in the file.
            if (m_impl->auditNoted.insert("mv2:" + oldK).second) {
                m_impl->auditPending.push_back(
                    file + ": both " + oldK + " and " + newK + " are set, "
                    "with different values. The new name wins (" + n->second +
                    "); delete the old line.");
            }
        }
    }

    std::set<std::string> known;
    for (size_t i = 0; i < g_auditKnownCount; ++i) known.insert(g_auditKnown[i]);
    std::string dead;
    int deadCount = 0;
    int deadShown = 0;
    for (const auto& kv : parsed) {
        if (known.count(kv.first) || movedOld.count(kv.first)) continue;
        if (!m_impl->auditNoted.insert("uk:" + kv.first).second) continue;
        // The separator belongs to the NAME, not to the iteration. It was
        // appended every time round while the name stopped after eight, so
        // a file with more than eight dead lines printed its first eight
        // and then a run of bare commas -- which is what the field log of
        // 2026-09-07 showed.
        //
        // The cap was the worse half. `parsed` is sorted, so all eight
        // slots went to advanced.* and every misplaced fix.* key was
        // invisible BY CONSTRUCTION. That is how an advanced.texture_lod_bias
        // written under [fix] stayed unread AND unreported through a whole
        // session of wondering why the picture was soft. Fill the line the
        // log can carry, and only then say there are more.
        if (dead.size() < 900) {
            if (deadShown) dead += ", ";
            dead += kv.first;
            ++deadShown;
        }
        ++deadCount;
    }
    if (deadCount) {
        if (deadCount > deadShown) dead += ", ...";
        m_impl->auditPending.push_back(
            file + ": " + std::to_string(deadCount) +
            " line(s) name settings this build does not read: " + dead +
            ". A typo or a retired setting -- those lines do nothing.");
    }
    // One place, after every push above. The flag is what lets a getter skip
    // the lock, so it must be up whenever a line waits: it stays up from an
    // earlier parse until auditFlush() drains the queue.
    if (!m_impl->auditPending.empty()) m_auditPending.store(true, std::memory_order_release);
}

void Config::auditFlush() const {
    // Lock-free while there is nothing to say, which is nearly always: this
    // runs on every getString, on whatever thread is reading.
    if (!m_auditPending.load(std::memory_order_acquire)) return;
    // The findings wait for the log: the first parse runs before Log::open,
    // and the first config read after it opens is soon enough. The flag stays
    // up meanwhile, so a closed log costs two loads per read and no lock.
    if (!Log::get().isOpen()) return;

    std::vector<std::string> lines;
    {
        // Exclusive, because this drains a queue that a parse on another
        // thread may be appending to. Re-checked here rather than trusted from
        // the flag: two threads can both get past the load above, and only the
        // first finds anything -- the second swaps out an empty vector.
        WriteGuard g(m_lock);
        if (m_impl) lines.swap(m_impl->auditPending);
        m_auditPending.store(false, std::memory_order_release);
    }
    // Written after the lock is gone: note() formats, takes the log's own spin
    // lock and may start its flusher thread, none of which belongs inside ours.
    for (const std::string& s : lines) {
        Log::get().note("%s", s.c_str());
    }
}

bool Config::reloadIfChanged() {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(m_path.c_str(), GetFileExInfoStandard, &data)) return false;
    // m_lastWrite is stamped by parse() under the lock, so it is read under it.
    FILETIME last;
    {
        ReadGuard g(m_lock);
        last = m_lastWrite;
    }
    if (CompareFileTime(&data.ftLastWriteTime, &last) == 0) return false;
    parse();
    return true;
}

std::string Config::getString(const char* key, const char* def) const {
    if (!runtimeProfileAllowsKey(key))
        return key && std::strncmp(key, "hotkey.", 7) == 0 ? "" : "off";
    // The audit's findings wait here for the log: the first parse runs before
    // Log::open, and the first config read after it opens is soon enough.
    auditFlush();
    {
        // The value is copied out under the shared lock. A reload swaps the map
        // and frees the old one, so nothing may still point into it once the
        // lock is released -- which is why this returns a std::string and not a
        // reference or a const char*.
        ReadGuard g(m_lock);
        if (m_impl) {
            const auto it = m_impl->values.find(key);
            if (it != m_impl->values.end()) return it->second;
        }
    }
    return std::string(def ? def : "");
}

std::string Config::requestedTemporalMode() const {
    ReadGuard g(m_lock);
    if (!m_impl) return "off";
    const auto it = m_impl->values.find("fix.temporal_aa");
    return it == m_impl->values.end() ? "off" : it->second;
}

// Said once per key per successful parse, and only once the log can take it.
//
// The getters below used to note a malformed value on EVERY read. A key read
// each frame -- most of the ones that matter -- wrote 90 to 180 identical lines
// a second until log.max_mb, after which the flight logged nothing at all.
//
// A parse clears the set, so an edit that leaves the value malformed is
// reported again, once. A note the log cannot take is not spent: note() does
// nothing before Log::open, and much is read before it (log.enabled, log.max_mb,
// log.buffer_mb, everything at start-up), so marking the key then would lose
// the line for good.
//
// Two threads reading the same key can both find it unsaid; emplace() decides,
// under the exclusive lock, which of them says it. A read that straddles a
// reload can be reported against the new parse's set. That costs at most one
// suppressed repeat, of a value that has just been replaced.
bool Config::firstNoteFor(const char* key) const {
    if (!Log::get().isOpen()) return false;
    {
        ReadGuard g(m_lock);
        if (!m_impl || m_impl->noteNoted.find(key) != m_impl->noteNoted.end()) return false;
    }
    WriteGuard g(m_lock);
    return m_impl && m_impl->noteNoted.emplace(key).second;
}

bool Config::getBool(const char* key, bool def) const {
    if (!runtimeProfileAllowsKey(key)) return false;
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
    //
    // Once per key per parse (firstNoteFor), not once per read.
    if (firstNoteFor(key)) {
        Log::get().note("%s: %s = \"%s\" is not a yes/no value, so the default (%s) is "
                        "being used. Write 1/0, true/false, yes/no or on/off.",
                        iniName(), key, v.c_str(), def ? "on" : "off");
    }
    return def;
}

void Config::set(const char* key, const char* value) {
    if (!key || !value) return;
    std::string k(key);
    for (char& c : k) c = static_cast<char>(tolower(c));
    WriteGuard g(m_lock);
    if (!m_impl) m_impl = new Impl();
    m_impl->values[k] = value;
    // A new value is a new thing to say about the key, as a parse would be.
    m_impl->noteNoted.erase(k);
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
    if (!runtimeProfileAllowsKey(key)) return 0;
    const std::string v = getString(key, "");
    if (v.empty()) return def;
    const char* s = v.c_str();
    char* end = nullptr;
    const long raw = strtol(s, &end, 0);
    if (!wholeValueParsed(s, end)) {
        if (firstNoteFor(key)) {
            Log::get().note("%s = \"%s\" is not a plain number; using %d. Everything "
                            "after the digits was ignored before this, which made a "
                            "typo read as a deliberate setting.", key, s, def);
        }
        return def;
    }
    return static_cast<int>(raw);
}

float Config::getFloat(const char* key, float def) const {
    if (!runtimeProfileAllowsKey(key)) return 0.0f;
    const std::string v = getString(key, "");
    if (v.empty()) return def;
    // A value that does not parse reads 0.0 silently, and 0.0 is a legitimate
    // setting for most of these -- so "I typed 2,75 in a comma locale" and "I
    // meant 0" are indistinguishable in the log and in the headset. strtof's
    // endptr tells them apart, so it is used.
    //
    // All of it, and finite (2026-09-29). This used to ask only that SOMETHING
    // parsed, so "2,75" read as 2 and "1.5x" as 1.5 -- the same silent
    // truncation getInt was cured of. And strtof accepts "nan" and "inf", which
    // no setting can use: NaN compares false against every bound a caller
    // checks, so it walks past the range test that would have caught a bad
    // number, and infinity turns the arithmetic after it into infinity.
    const char* s = v.c_str();
    char* end = nullptr;
    const float out = strtof(s, &end);
    if (!wholeValueParsed(s, end) || !std::isfinite(out)) {
        if (firstNoteFor(key)) {
            Log::get().note("%s = \"%s\" is not a number; using %g. If you meant a "
                            "decimal, use a point rather than a comma.", key, s, def);
        }
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
    if (!runtimeProfileAllowsKey(key)) return 0;
    const std::string v = getString(key, "");
    if (v.empty()) return def;
    const char* s = v.c_str();
    char* end = nullptr;
    const long raw = strtol(s, &end, 0);
    if (!wholeValueParsed(s, end)) {
        if (firstNoteFor(key)) {
            Log::get().note("%s = \"%s\" is not a number; using %d.", key, s, def);
        }
        return def;
    }
    if (raw < lo || raw > hi) {
        const long c = raw < lo ? lo : hi;
        if (firstNoteFor(key)) {
            Log::get().note("%s = %ld is outside %d..%d, so %ld is being used.",
                            key, raw, lo, hi, c);
        }
        return static_cast<int>(c);
    }
    return static_cast<int>(raw);
}

}  // namespace edvr
