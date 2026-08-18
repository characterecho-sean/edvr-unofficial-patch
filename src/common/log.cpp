#include "log.h"

// Set by build.bat from `git describe` for the two shipped DLLs. The test
// binaries that link this file compile without the define, and the fallback
// says what it is rather than impersonating a release.
#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unversioned test build"
#endif

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "config.h"

namespace edvr {
namespace {

// Past this we drop lines and count them rather than growing without bound.
constexpr size_t kBufferCapBytes = 1u * 1024u * 1024u;
constexpr DWORD  kFlushIntervalMs = 250;

// The link time of the module this code is compiled into, read from its own PE
// header. This is the identity a binary carries wherever it is copied, and the
// log states it so "which build produced this log" is never again a diagnosis
// (6ao: the anchor hunt was debugged three times in source that had never
// flown). Zero when the header is not as expected -- "no stamp", not an error.
// Kept textually identical in both repos' log.cpp; cross-diff when touching.
uint32_t moduleLinkStamp() {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&moduleLinkStamp), &mod) ||
        !mod) {
        return 0;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(mod);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->FileHeader.TimeDateStamp;
}

}  // namespace

int64_t qpcNow() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

int64_t qpcFrequency() {
    static int64_t freq = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return freq;
}

uint64_t fnv1a64(const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

struct Log::Impl {
    std::vector<char> buf[2];
    std::atomic<int>  active{0};
    std::atomic_flag  spin = ATOMIC_FLAG_INIT;
    std::thread       flusher;
    std::atomic<bool> stop{false};
    std::atomic<bool> started{false};
    HANDLE            file = INVALID_HANDLE_VALUE;
};

Log& Log::get() {
    static Log instance;
    return instance;
}

Log::~Log() { close(); }

void Log::lock() {
    while (m_impl->spin.test_and_set(std::memory_order_acquire)) {
        YieldProcessor();
    }
}

void Log::unlock() { m_impl->spin.clear(std::memory_order_release); }

bool Log::open(const std::wstring& dir, const wchar_t* tag) {
    if (m_open) return true;
    if (!Config::get().getBool("log.enabled", true)) return false;

    const int maxMb = Config::get().getInt("log.max_mb", 4);
    m_maxBytes = maxMb > 0 ? static_cast<uint64_t>(maxMb) * 1024ull * 1024ull : 0ull;

    m_impl = new Impl();
    m_impl->buf[0].reserve(64 * 1024);
    m_impl->buf[1].reserve(64 * 1024);
    m_dir = dir;

    // Created here rather than at config time, so logging turned off leaves no
    // empty directory behind.
    ensureDirectory(dir);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\edvr_%s_%04u%02u%02u_%02u%02u%02u.log",
                 dir.c_str(), tag, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond);

    m_impl->file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_impl->file == INVALID_HANDLE_VALUE) {
        delete m_impl;
        m_impl = nullptr;
        return false;
    }

    m_open = true;
    note("EDVR log -- unofficial VR fixes for Elite Dangerous: Odyssey");
    // The version names the git tag the DLL was built from, straight from
    // `git describe` at build time (see build.bat). It exists because the link
    // stamp alone cannot answer "which release is this" without correlating
    // hex against tag dates by hand -- which is what triaging a field log used
    // to start with. A -N-g or -dirty suffix means a dev build and says so.
    // TimeDateStamp is seconds since 1970 UTC unless a reproducible-build flag
    // replaced it with a hash; the hex is the exact identity either way, and
    // it stays printed because two builds of one tag are still two builds.
    const uint32_t stamp = moduleLinkStamp();
    if (stamp != 0) {
        ULARGE_INTEGER t;
        t.QuadPart = 116444736000000000ull +
                     static_cast<uint64_t>(stamp) * 10000000ull;
        FILETIME ft;
        ft.dwLowDateTime  = t.LowPart;
        ft.dwHighDateTime = t.HighPart;
        SYSTEMTIME bs{};
        if (FileTimeToSystemTime(&ft, &bs)) {
            note("version %s (build %08X) -- this DLL was linked %04u-%02u-%02u "
                 "%02u:%02u:%02u UTC",
                 EDVR_VERSION_STRING, stamp, bs.wYear, bs.wMonth, bs.wDay,
                 bs.wHour, bs.wMinute, bs.wSecond);
        } else {
            note("version %s (build %08X)", EDVR_VERSION_STRING, stamp);
        }
    } else {
        note("version %s", EDVR_VERSION_STRING);
    }
    note("If you are reporting a problem, paste this whole file.");
    return true;
}

void Log::startFlusherOnce() {
    // Never called from DllMain: the first line is written from an export call,
    // well after loader lock.
    bool expected = false;
    if (!m_impl->started.compare_exchange_strong(expected, true)) return;
    m_impl->flusher = std::thread([this] { flusherMain(); });
}

void Log::flusherMain() {
    SetThreadDescription(GetCurrentThread(), L"edvr-log");
    while (!m_impl->stop.load(std::memory_order_relaxed)) {
        Sleep(kFlushIntervalMs);
        lock();
        const int full = m_impl->active.load(std::memory_order_relaxed);
        m_impl->active.store(full ^ 1, std::memory_order_relaxed);
        unlock();
        writeBuffer(full);
    }
}

void Log::writeBuffer(int index) {
    std::vector<char>& b = m_impl->buf[index];
    if (b.empty()) return;

    if (m_maxBytes && m_bytesWritten >= m_maxBytes) {
        if (!m_capped) {
            m_capped = true;
            const char msg[] = "\r\n[edvr] log size cap reached; nothing further will "
                               "be written. Raise log.max_mb to change this.\r\n";
            DWORD n = 0;
            WriteFile(m_impl->file, msg, sizeof(msg) - 1, &n, nullptr);
            FlushFileBuffers(m_impl->file);
        }
        b.clear();
        return;
    }

    DWORD written = 0;
    WriteFile(m_impl->file, b.data(), static_cast<DWORD>(b.size()), &written, nullptr);
    m_bytesWritten += written;
    b.clear();
}

void Log::append(const char* text, size_t bytes) {
    lock();
    std::vector<char>& b = m_impl->buf[m_impl->active.load(std::memory_order_relaxed)];
    if (b.size() + bytes > kBufferCapBytes) {
        unlock();
        m_dropped++;
        return;
    }
    b.insert(b.end(), text, text + bytes);
    unlock();
}

void Log::note(const char* fmt, ...) {
    if (!m_open) return;
    startFlusherOnce();

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[1200];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u.%03u] ",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (n < 0) return;

    va_list ap;
    va_start(ap, fmt);
    // Room for the marker is reserved up front, not found afterwards.
    //
    // Formatting into the whole buffer and then looking for space left exactly
    // one byte, so the "...[truncated]" marker could never render -- a single
    // space was appended instead, and a truncated line looked like an ordinary
    // one. Messages lose the marker's width of capacity; a line long enough to
    // notice that is a line being truncated anyway.
    static const char kMark[] = " ...[truncated]";
    const int kMarkLen = static_cast<int>(sizeof(kMark)) - 1;
    const int m = _vsnprintf_s(line + n, sizeof(line) - n - 3 - kMarkLen, _TRUNCATE,
                               fmt, ap);
    va_end(ap);
    if (m > 0) {
        n += m;
    } else if (m < 0) {
        // Truncated, not failed.
        //
        // _vsnprintf_s with _TRUNCATE returns -1 when the text did not fit,
        // having already written as much as did and NUL-terminated it. Testing
        // `m > 0` therefore threw the whole message away and left a bare
        // timestamp -- and the messages long enough to hit this are the ones
        // carrying paths and diagnostics somebody is reading off a support log.
        n += static_cast<int>(strlen(line + n));
        const int room = static_cast<int>(sizeof(line)) - n - 3;
        const int take = kMarkLen < room ? kMarkLen : room;
        if (take > 0) {
            memcpy(line + n, kMark, static_cast<size_t>(take));
            n += take;
        }
    }

    line[n++] = '\r';
    line[n++] = '\n';
    append(line, static_cast<size_t>(n));
}

void Log::detachDuringProcessExit() {
    if (!m_open || !m_impl) return;

    // Cleared first so every later note() is a no-op, including any the guard
    // filter would emit, and so close() at CRT teardown returns immediately.
    m_open = false;
    m_impl->stop.store(true);

    // Unlocked and best effort: a terminated flusher may still hold the spinlock,
    // and blocking here would hang the process rather than lose a few lines.
    __try {
        for (int i = 0; i < 2; ++i) {
            std::vector<char>& b = m_impl->buf[i];
            if (b.empty()) continue;
            DWORD written = 0;
            WriteFile(m_impl->file, b.data(), static_cast<DWORD>(b.size()), &written,
                      nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Buffers were mid-update when their writer was killed. Nothing to
        // recover; everything flushed before this is already on disk.
    }

    if (m_impl->file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(m_impl->file);
        CloseHandle(m_impl->file);
        m_impl->file = INVALID_HANDLE_VALUE;
    }

    // m_impl is deliberately leaked: freeing it would run a std::thread
    // destructor on a dead thread and call the allocator, either of which can
    // fault if the terminated flusher held the heap lock.
}

void Log::close() {
    if (!m_open || !m_impl) return;
    m_open = false;

    if (m_impl->started.load()) {
        m_impl->stop.store(true);
        if (m_impl->flusher.joinable()) m_impl->flusher.join();
    }
    writeBuffer(0);
    writeBuffer(1);
    if (m_impl->file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(m_impl->file);
        CloseHandle(m_impl->file);
        m_impl->file = INVALID_HANDLE_VALUE;
    }
    delete m_impl;
    m_impl = nullptr;
}

}  // namespace edvr
