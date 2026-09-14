#pragma once

// CPU-only claim budget for the OpenXR order census.  The two banks are
// deliberately independent: claims already in flight during startup stay in
// the startup bank when the phase changes to VR.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace edvr {

enum class VrCensusPhase : unsigned { Startup = 0, Vr = 1 };

template <std::size_t EventCount>
class VrCensusBudget final {
    static_assert(EventCount > 0, "VrCensusBudget needs at least one event");

    std::array<std::atomic<std::uint32_t>, EventCount> startup_{};
    std::array<std::atomic<std::uint32_t>, EventCount> vr_{};
    std::atomic<unsigned> phase_{static_cast<unsigned>(VrCensusPhase::Startup)};

    static std::atomic<std::uint32_t>& bank(
        std::array<std::atomic<std::uint32_t>, EventCount>& startup,
        std::array<std::atomic<std::uint32_t>, EventCount>& vr,
        VrCensusPhase phase, unsigned event) noexcept {
        return phase == VrCensusPhase::Startup ? startup[event] : vr[event];
    }

public:
    VrCensusBudget() noexcept {
        // Explicit for both process-static production storage and automatic
        // test instances, independent of atomic default-constructor semantics.
        for (auto& count : startup_) count.store(0, std::memory_order_relaxed);
        for (auto& count : vr_) count.store(0, std::memory_order_relaxed);
    }
    VrCensusBudget(const VrCensusBudget&) = delete;
    VrCensusBudget& operator=(const VrCensusBudget&) = delete;

    // Switch permanently to the VR bank. Exactly one caller reports success.
    bool beginVr() noexcept {
        unsigned expected = static_cast<unsigned>(VrCensusPhase::Startup);
        return phase_.compare_exchange_strong(
            expected, static_cast<unsigned>(VrCensusPhase::Vr),
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    VrCensusPhase phase() const noexcept {
        return static_cast<VrCensusPhase>(phase_.load(std::memory_order_acquire));
    }

    // Select the phase once on entry, then claim a 1-based sample in that
    // phase's bank. Invalid events and zero limits never consume a claim.
    bool take(unsigned event, std::uint32_t limit, VrCensusPhase& selected,
              std::uint32_t& sample) noexcept {
        const VrCensusPhase chosen = phase();
        selected = chosen;
        sample = 0;
        if (event >= EventCount || limit == 0) return false;
        auto& count = bank(startup_, vr_, chosen, event);
        std::uint32_t current = count.load(std::memory_order_relaxed);
        for (;;) {
            if (current >= limit) return false;
            const std::uint32_t next = current + 1;
            if (count.compare_exchange_weak(current, next,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
                sample = next;
                return true;
            }
        }
    }
};

} // namespace edvr
