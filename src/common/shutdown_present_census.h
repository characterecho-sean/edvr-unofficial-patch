#pragma once

#include "shutdown_census_api.h"
#include <atomic>
#include <cstdint>
#include <limits>

namespace edvr {

// CPU-only observation windows around the owned Present boundary.  The class
// never calls into graphics or VR code; callers provide the exact boundary
// point at which a successful Present has been forwarded.
class ShutdownPresentCensus final {
public:
    static constexpr std::uint32_t kMaxWindows = 64;

    ShutdownPresentCensus() = default;
    ShutdownPresentCensus(const ShutdownPresentCensus&) = delete;
    ShutdownPresentCensus& operator=(const ShutdownPresentCensus&) = delete;

    // The caller supplies the current enable/readiness gate. Rejected calls
    // leave the active and completed window state untouched.
    std::uint64_t begin(std::uint32_t version, bool enabledAndReady) noexcept {
        if (version != kShutdownCensusVersion || !enabledAndReady ||
            activeToken_.load(std::memory_order_acquire) != 0)
            return 0;

        AcquireSRWLockExclusive(&lock_);
        if (activeToken_.load(std::memory_order_relaxed) != 0 ||
            acceptedWindows_ >= kMaxWindows) {
            ReleaseSRWLockExclusive(&lock_);
            return 0;
        }
        const std::uint64_t token = nextToken_++;
        if (token == 0) {
            ReleaseSRWLockExclusive(&lock_);
            return 0;
        }
        ShutdownCensusSnapshot snapshot{};
        snapshot.size = sizeof(snapshot);
        snapshot.version = kShutdownCensusVersion;
        snapshot.token = token;
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc); // ordered with all state under the lock
        snapshot.beginQpc = qpc.QuadPart;
        active_ = snapshot;
        ++acceptedWindows_;
        activeToken_.store(token, std::memory_order_release);
        ReleaseSRWLockExclusive(&lock_);
        return token;
    }

    BOOL end(std::uint64_t token, ShutdownCensusSnapshot* result) noexcept {
        if (!result || result->size != sizeof(ShutdownCensusSnapshot) ||
            result->version != kShutdownCensusVersion || token == 0 ||
            activeToken_.load(std::memory_order_acquire) != token)
            return FALSE;

        AcquireSRWLockExclusive(&lock_);
        if (activeToken_.load(std::memory_order_relaxed) != token) {
            ReleaseSRWLockExclusive(&lock_);
            return FALSE;
        }
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc); // ordered with the last sample under lock
        active_.endQpc = qpc.QuadPart;
        *result = active_;
        active_ = {};
        activeToken_.store(0, std::memory_order_release);
        ReleaseSRWLockExclusive(&lock_);
        return TRUE;
    }

    // Fast path for normal Presents outside a window: no QPC or mutex work.
    void notePresent() noexcept {
        const std::uint64_t token = activeToken_.load(std::memory_order_acquire);
        if (!token) return;
        AcquireSRWLockExclusive(&lock_);
        if (activeToken_.load(std::memory_order_acquire) != token) {
            ReleaseSRWLockExclusive(&lock_);
            return;
        }
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        recordLocked(qpc.QuadPart, GetCurrentThreadId());
        ReleaseSRWLockExclusive(&lock_);
    }

private:
    void recordLocked(std::int64_t qpc, std::uint32_t thread) noexcept {
        if (active_.samples == (std::numeric_limits<std::uint64_t>::max)()) {
            active_.saturated = 1;
            return;
        }
        if (active_.samples == 0) {
            active_.firstQpc = qpc;
            active_.firstThread = thread;
            active_.lastQpc = qpc;
            active_.lastThread = thread;
        } else {
            active_.lastQpc = qpc;
            active_.lastThread = thread;
            if (active_.firstThread != thread) active_.mixedThreads = 1;
        }
        ++active_.samples;
    }

    SRWLOCK lock_ = SRWLOCK_INIT;
    std::atomic<std::uint64_t> activeToken_{0};
    ShutdownCensusSnapshot active_{};
    std::uint64_t nextToken_ = 1;
    std::uint32_t acceptedWindows_ = 0;
};

} // namespace edvr
