#include "log.h"

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
    note("EDVR log -- per-eye brightness fix for Elite Dangerous in VR");
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
    const int m = _vsnprintf_s(line + n, sizeof(line) - n - 3, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m > 0) n += m;

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
