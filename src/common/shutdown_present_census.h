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
    ~ShutdownPresentCensus() {
        if (ownerHandle_) CloseHandle(ownerHandle_);
    }
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
        pollOwnerLocked();
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc); // ordered with all state under the lock
        snapshot.beginQpc = qpc.QuadPart;
        snapshot.renderBegin = render_;
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
        pollOwnerLocked();
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc); // ordered with the last sample under lock
        active_.endQpc = qpc.QuadPart;
        active_.renderEnd = render_;
        *result = active_;
        active_ = {};
        activeToken_.store(0, std::memory_order_release);
        ReleaseSRWLockExclusive(&lock_);
        return TRUE;
    }

    // Whole-Present activity is cumulative and deliberately includes failed
    // and TEST Presents. The hook gates these calls on census enablement.
    void enterPresent() noexcept {
        AcquireSRWLockExclusive(&lock_);
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        const auto max = (std::numeric_limits<std::uint64_t>::max)();
        if (render_.enteredPresents == max)
            render_.activityInvalid = 1;
        else
            ++render_.enteredPresents;
        if (render_.activePresents == max)
            render_.activityInvalid = 1;
        else
            ++render_.activePresents;
        render_.lastEnterQpc = qpc.QuadPart;
        const DWORD thread = GetCurrentThreadId();
        render_.lastEnterThread = thread;
        if (ownerObserved_) markOwnerChangeLocked(thread);
        ReleaseSRWLockExclusive(&lock_);
    }

    void leavePresent() noexcept {
        AcquireSRWLockExclusive(&lock_);
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        const auto max = (std::numeric_limits<std::uint64_t>::max)();
        if (render_.activePresents == 0)
            render_.activityInvalid = 1;
        else
            --render_.activePresents;
        if (render_.exitedPresents == max)
            render_.activityInvalid = 1;
        else
            ++render_.exitedPresents;
        render_.lastExitQpc = qpc.QuadPart;
        render_.lastExitThread = GetCurrentThreadId();
        ReleaseSRWLockExclusive(&lock_);
    }

    // Record the first owned Present caller without ever reopening or
    // replacing its retained thread handle.
    void noteOwner() noexcept {
        AcquireSRWLockExclusive(&lock_);
        const DWORD thread = GetCurrentThreadId();
        if (!ownerObserved_) {
            ownerObserved_ = true;
            render_.ownerThread = thread;
            HANDLE duplicate = nullptr;
            if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                                GetCurrentProcess(), &duplicate, SYNCHRONIZE,
                                FALSE, 0)) {
                ownerHandle_ = duplicate;
                pollOwnerLocked();
            } else {
                render_.ownerState = ShutdownOwnerState::Unavailable;
                render_.ownerError = GetLastError();
            }
        } else markOwnerChangeLocked(thread);
        ReleaseSRWLockExclusive(&lock_);
    }

    // The hook gates this method on census enablement so disabled sessions do
    // no work. The token and timestamp are linearized under the same lock, so
    // the sample belongs to whichever window owns this service point.
    void notePresent() noexcept {
        AcquireSRWLockExclusive(&lock_);
        const std::uint64_t token = activeToken_.load(std::memory_order_relaxed);
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        render_.lastServiceQpc = qpc.QuadPart;
        if (token) recordLocked(qpc.QuadPart, GetCurrentThreadId());
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

    void pollOwnerLocked() noexcept {
        if (!ownerHandle_) return;
        const DWORD result = WaitForSingleObject(ownerHandle_, 0);
        if (result == WAIT_TIMEOUT) {
            render_.ownerState = ShutdownOwnerState::Alive;
            render_.ownerError = 0;
        } else if (result == WAIT_OBJECT_0) {
            render_.ownerState = ShutdownOwnerState::Exited;
            render_.ownerError = 0;
        } else {
            render_.ownerState = ShutdownOwnerState::Unavailable;
            render_.ownerError = result == WAIT_FAILED ? GetLastError() : result;
        }
    }

    void markOwnerChangeLocked(DWORD thread) noexcept {
        pollOwnerLocked();
        if (thread != render_.ownerThread || render_.ownerState == ShutdownOwnerState::Exited)
            render_.ownerChanged = 1;
    }

    SRWLOCK lock_ = SRWLOCK_INIT;
    std::atomic<std::uint64_t> activeToken_{0};
    ShutdownCensusSnapshot active_{};
    ShutdownRenderSnapshot render_{};
    HANDLE ownerHandle_ = nullptr;
    bool ownerObserved_ = false;
    std::uint64_t nextToken_ = 1;
    std::uint32_t acceptedWindows_ = 0;
};

} // namespace edvr
