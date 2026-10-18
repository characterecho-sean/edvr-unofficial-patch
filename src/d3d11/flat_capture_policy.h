#pragma once
#include <atomic>
#include <cstdint>

enum class FlatCaptureTier : unsigned { None, General, Full };

enum class FlatDrawCaptureRoute : uint8_t { None, Vr, General, Full, Passive };

// Shift retains the full diagnostic request. Ctrl adds a passive restart for
// keyboard bindings that do not themselves require Ctrl; other bindings keep
// their existing route.
inline FlatDrawCaptureRoute flatDrawCaptureRoute(bool requested, bool flatProfile,
                                                bool passiveKeyboardBinding, bool shiftHeld,
                                                bool ctrlHeld) noexcept {
    if (!requested) return FlatDrawCaptureRoute::None;
    if (!flatProfile) return FlatDrawCaptureRoute::Vr;
    if (shiftHeld) return FlatDrawCaptureRoute::Full;
    if (ctrlHeld && passiveKeyboardBinding) return FlatDrawCaptureRoute::Passive;
    return FlatDrawCaptureRoute::General;
}

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
    FlatCaptureTier pending() const {
        return static_cast<FlatCaptureTier>(pending_.load(std::memory_order_acquire));
    }
};

inline bool flatCaptureBulk(FlatCaptureTier tier) { return tier == FlatCaptureTier::Full; }

struct FlatCaptureBudget {
    uint64_t milliseconds;
    uint32_t usefulFrames;
};

// Ordinary reports sample the problem already on screen. Keeping the observers
// running for two minutes costs driver/state work even without bulk exports.
inline FlatCaptureBudget flatCaptureBudget(FlatCaptureTier tier) {
    return tier == FlatCaptureTier::General ? FlatCaptureBudget{1000, 3} : FlatCaptureBudget{120000, 12000};
}
