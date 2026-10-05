#pragma once
#include <atomic>

enum class FlatCaptureTier : unsigned { None, General, Full };

// Key requests may overlap before Present consumes them. A general request
// must never downgrade a pending explicit full capture.
class FlatCaptureRequest {
    std::atomic<unsigned> pending_{0};
public:
    void request(bool full) {
        const unsigned wanted = static_cast<unsigned>(full ? FlatCaptureTier::Full : FlatCaptureTier::General);
        unsigned prior = pending_.load(std::memory_order_relaxed);
        while (prior < wanted && !pending_.compare_exchange_weak(prior, wanted,
                std::memory_order_release, std::memory_order_relaxed)) {}
    }
    FlatCaptureTier take() {
        return static_cast<FlatCaptureTier>(pending_.exchange(0, std::memory_order_acq_rel));
    }
};

inline bool flatCaptureBulk(FlatCaptureTier tier) { return tier == FlatCaptureTier::Full; }
