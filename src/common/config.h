// Hot-reloadable key=value config.
//
// The user is wearing a headset and cannot see a text editor, so every tunable
// lives here and is re-read when the file's write time changes. Reload is
// polled from the frame loop, not watched, to keep the cost to one cheap
// GetFileAttributesEx every N frames.
//
// Threads: any getter may run on any thread while reloadIfChanged() runs on
// another. The OpenXR owner thread's deferred frame end reads
// advanced.app_gpu_timing on its own thread while the render thread reloads.
// Reads take a shared lock and copy the value out; a reload parses the file
// outside the lock and takes it exclusively only to swap the new map in.
#pragma once

#include <windows.h>  // FILETIME, SRWLOCK

#include <atomic>
#include <string>

#include "ini_name.h"
#include "runtime_profile.h"

namespace edvr {

class Config {
public:
    static Config& get();

    // Looks for the settings file next to the host module, then next to the
    // .exe: edvr-flat.ini first under the flat profile, then edvr.ini (the VR
    // profile's file, and the flat profile's whole fallback while edvr-flat.ini
    // does not exist yet). One file is read, never a mix of the two.
    // logDir() is the ini's log.dir, else <exe dir>\edvr_logs, which the
    // environment may move: EDVR_LOG_DIR names another directory, and
    // EDVR_LOG_DIR_FOR, when set, applies it only to exes in that directory
    // (the test runner's way of giving each rig's proxies their own logs).
    void init(const std::wstring& moduleDir);

    // The name of the settings file this process opened -- "edvr-flat.ini" or
    // "edvr.ini" -- for every message that tells somebody which file to open or
    // says which was written (ini_name.h says why none may spell it). It comes
    // from the path init() chose, so a flat install that fell back to edvr.ini
    // says edvr.ini, because that is the file it read. Before init() there is
    // no path, and the profile answers. Inline, so a test that stubs some of
    // Config's members and compiles a module which names the file still links.
    const char* iniName() const {
        return m_path.empty() ? (runtimeFlatProfile() ? kIniNameFlat : kIniNameVr)
                              : iniNameOfPath(m_path);
    }

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
    // Diagnostic intent, before scope filtering; never enables a feature.
    std::string requestedTemporalMode() const;

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
                        const char* const (*movedOldNew)[3], size_t movedCount);

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
    // Called with m_lock held exclusively; queues findings, never logs them.
    void auditResolve(void* parsedMap);
    // Writes queued audit findings once the log is open. Takes m_lock itself,
    // and only when there is something to write.
    void auditFlush() const;
    // True when the note about a malformed or out-of-range value of `key`
    // should be written now: the first time since the last successful parse
    // that the log could take it. Takes m_lock; call it with none held.
    bool firstNoteFor(const char* key) const;

    struct Impl;
    Impl*        m_impl = nullptr;
    std::wstring m_path;
    std::wstring m_logDir;
    FILETIME     m_lastWrite{};

    // The one lock over everything m_impl points at, and over m_lastWrite.
    // It lives here rather than in Impl so a read before init() is safe.
    //
    // Shared for a lookup, which copies the value out before releasing it;
    // exclusive for anything that writes: parse's swap and audit bookkeeping,
    // the audit flush's drain, the once-per-parse note set, and set().
    //
    // SRW locks are not recursive, so nothing that can call back into Config
    // runs while it is held -- no getter, no Log call. Log::open reads Config,
    // and Config never calls Log::open; Log::note takes only the log's own
    // spin lock and reads nothing here.
    mutable SRWLOCK m_lock = SRWLOCK_INIT;

    // True while Impl::auditPending holds lines. Written under the exclusive
    // lock; read without it, so a getter with nothing to flush -- nearly all
    // of them -- costs one load and takes no lock.
    mutable std::atomic<bool> m_auditPending{false};
};

// Directory containing the given loaded module, without trailing slash.
std::wstring moduleDirectory(void* hModule);
// Directory containing the running .exe.
std::wstring executableDirectory();
bool ensureDirectory(const std::wstring& path);

}  // namespace edvr
