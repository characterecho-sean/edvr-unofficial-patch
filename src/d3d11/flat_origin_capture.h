#pragma once
#include <atomic>

namespace edvr {
// Sampled once at the first owned Present after config/profile selection.
// Uninitialized means off. Reloads and profile switches cannot enable it;
// changing diag.flat_origin_capture requires restarting the game in flat.
class FlatOriginCaptureControl {
    std::atomic<unsigned> mode_{0}; // 0 unset, 1 off, 2 on
public:
    bool configured() const noexcept { return mode_.load(std::memory_order_acquire) != 0; }
    bool enabled() const noexcept { return mode_.load(std::memory_order_acquire) == 2; }
    void initialize(bool enabled) noexcept {
        unsigned expected = 0;
        mode_.compare_exchange_strong(expected, enabled ? 2u : 1u, std::memory_order_release);
    }
};
inline FlatOriginCaptureControl flatOriginCaptureControl;
inline bool flatOriginCaptureEnabled() noexcept { return flatOriginCaptureControl.enabled(); }
inline void flatOriginCaptureInitialize(bool enabled) noexcept { flatOriginCaptureControl.initialize(enabled); }
} // namespace edvr
