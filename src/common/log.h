// EDVR log.
//
// Plain text, one timestamped line per event. It records which game build is
// running, whether the fix installed, which shader it settled on, and whether
// it engaged -- which is everything needed to make a bug report actionable, and
// nothing else.
//
// Text rather than a binary format on purpose: the support path for this is
// somebody opening the file and pasting it into a forum post, and that should
// not require a decoder.
//
// Writes go to a double-buffered ring drained by a background thread, because
// the alternative is touching the filesystem from the render thread at 90Hz.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>

namespace edvr {

class Log {
public:
    static Log& get();

    // dir is created if needed. Safe to call more than once.
    bool open(const std::wstring& dir, const wchar_t* tag);

    // Normal teardown: stops and joins the flusher, releases everything.
    void close();

    // Teardown for DLL_PROCESS_DETACH during process termination.
    //
    // By then Windows has terminated every other thread, possibly while one
    // held our spinlock or the process heap lock. So this must never join a
    // thread, never take the spinlock, and never free heap. It writes what it
    // can and leaves the rest for the OS to reclaim.
    void detachDuringProcessExit();

    void note(const char* fmt, ...);

    bool isOpen() const { return m_open; }
    uint64_t dropped() const { return m_dropped; }
    const std::wstring& dir() const { return m_dir; }

private:
    Log() = default;
    ~Log();
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    void lock();
    void unlock();
    void startFlusherOnce();
    void flusherMain();
    void writeBuffer(int index);
    void append(const char* text, size_t bytes);

    struct Impl;
    Impl* m_impl = nullptr;

    std::wstring m_dir;
    volatile uint64_t m_dropped = 0;
    bool m_open = false;

    // Hard ceiling, so a long session cannot fill a disk.
    uint64_t m_maxBytes = 0;      // 0 = unlimited
    uint64_t m_bytesWritten = 0;
    bool     m_capped = false;
};

uint64_t fnv1a64(const void* data, size_t bytes);
int64_t  qpcNow();
int64_t  qpcFrequency();

}  // namespace edvr
