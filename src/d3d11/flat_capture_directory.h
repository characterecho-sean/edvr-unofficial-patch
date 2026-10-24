#pragma once
// Fail-closed, process-unique directory creation for the flat capture classes (#105).
// A per-instance serial let two freshly built captures armed in the same millisecond of one
// process share <timestamp>_<pid>_1, so their frame files mixed. Each arm now takes an id from
// one process-wide counter AND creates its directory with CreateDirectoryW, taking the next id
// when the name already exists, so two arms can never share a directory whatever the clock does.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>

namespace edvr {

// Test hook: when set, names are produced but nothing touches the disk.
inline bool flatDrawCaptureNoDirectoryWrites = false;

inline uint64_t flatCaptureNextId() {
    static std::atomic<uint64_t> next{0};
    return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

// Creates root\<leaf(id)> as a directory that did not exist before. `root` must already exist.
// leaf is called with a fresh process-wide id per attempt. On success `out` is the full path.
// On any other failure `out` holds the last attempted path and false is returned. After
// maxAttempts collisions it fails rather than reuse a directory.
template <class LeafFn>
inline bool createFreshCaptureDirectory(const std::wstring& root, LeafFn&& leaf, std::wstring& out,
                                        unsigned maxAttempts = 64) {
    for (unsigned attempt = 0; attempt < maxAttempts; ++attempt) {
        out = root + L"\\" + leaf(flatCaptureNextId());
        if (flatDrawCaptureNoDirectoryWrites) return true;
        if (CreateDirectoryW(out.c_str(), nullptr)) return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
}

}  // namespace edvr
