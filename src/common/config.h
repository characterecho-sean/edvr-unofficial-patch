// Hot-reloadable key=value config.
//
// The user is wearing a headset and cannot see a text editor, so every tunable
// lives here and is re-read when the file's write time changes. Reload is
// polled from the frame loop, not watched, to keep the cost to one cheap
// GetFileAttributesEx every N frames.
#pragma once

#include <windows.h>  // FILETIME

#include <string>

namespace edvr {

class Config {
public:
    static Config& get();

    // Looks for edvr.ini next to the host module, then next to the .exe.
    void init(const std::wstring& moduleDir);

    // Returns true if the file changed and values were re-read.
    bool reloadIfChanged();

    bool        getBool(const char* key, bool def) const;
    int         getInt(const char* key, int def) const;
    float       getFloat(const char* key, float def) const;
    // getInt, with bounds. Prefer this for anything a wrong value can make
    // behave as though the setting were absent -- which is most of them, and
    // is the failure that is hardest to attribute from a log.
    int         getIntInRange(const char* key, int def, int lo, int hi) const;
    std::string getString(const char* key, const char* def) const;

    // The config audit's data: every key this build reads or documents
    // (lowercase), and the moved-from map ({old, new} dotted names) parsed at
    // build time from edvr.ini's own annotations. Registered by
    // config_audit.cpp -- compiled into the DLLs only -- at static init, so
    // every parse can (a) read a moved key from its OLD location when the new
    // one is absent (hand-copied DLLs meet old-layout inis all the time; that
    // exact meeting shifted the whole scanner UI on 2026-08-27), and (b) name
    // any line the build does not read, instead of ignoring it silently.
    // Binaries that never register run without the audit, nothing else
    // changes. Tests register small fixture tables of their own.
    void setAuditTables(const char* const* knownLower, size_t knownCount,
                        const char* const (*movedOldNew)[2], size_t movedCount);

    // Set a value in memory, without touching the file.
    //
    // For tests, which need to ask what a module does when a setting CHANGES --
    // the gate freezing its latch on fix.head_offset_gate = 0 was a real
    // defect, and reproducing it by writing an ini and waiting for a write-time
    // poll would test the file watcher rather than the gate.
    //
    // Not used by the DLLs: a setting the game can change behind the file would
    // make the log and the ini disagree about what is running.
    void set(const char* key, const char* value);

    const std::wstring& path() const { return m_path; }
    const std::wstring& logDir() const { return m_logDir; }

private:
    Config() = default;
    void parse();
    void auditResolve(void* parsedMap);
    void auditFlush() const;

    struct Impl;
    Impl*        m_impl = nullptr;
    std::wstring m_path;
    std::wstring m_logDir;
    FILETIME     m_lastWrite{};
};

// Directory containing the given loaded module, without trailing slash.
std::wstring moduleDirectory(void* hModule);
// Directory containing the running .exe.
std::wstring executableDirectory();
bool ensureDirectory(const std::wstring& path);

}  // namespace edvr
