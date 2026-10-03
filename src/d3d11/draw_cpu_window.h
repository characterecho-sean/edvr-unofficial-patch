#pragma once

#include <cstdint>

namespace edvr::draw_cpu {

// Aggregates the already-clocked draws for the existing hook-CPU estimate.
// The frame numerator uses the same frame-level clamp as perf_monitor.cpp;
// the window mean divides unscaled own ticks by observed timed draws because
// the fixed 1/64 selection cancels for a per-draw mean.
struct Window final {
    std::uint64_t frameTimedDraws = 0;
    std::uint64_t windowTimedDraws = 0;
    std::int64_t windowOwnTicks = 0;

    void noteDraw(std::int64_t wholeTicks) noexcept {
        if (wholeTicks > 0) ++frameTimedDraws;
    }

    void closeFrame(bool validSampleFrame, std::int64_t positiveWholeTicks,
                    std::int64_t positiveRealTicks) noexcept {
        if (validSampleFrame) {
            windowTimedDraws += frameTimedDraws;
            const std::int64_t ownTicks = positiveWholeTicks - positiveRealTicks;
            if (ownTicks > 0) windowOwnTicks += ownTicks;
        }
        frameTimedDraws = 0;
    }

    bool hasTimedDraws() const noexcept { return windowTimedDraws != 0; }

    double meanMs(std::uint64_t qpcFrequency) const noexcept {
        if (!windowTimedDraws || !qpcFrequency) return 0.0;
        return static_cast<double>(windowOwnTicks) * 1000.0 /
               (static_cast<double>(qpcFrequency) * static_cast<double>(windowTimedDraws));
    }

    void resetWindow() noexcept {
        windowTimedDraws = 0;
        windowOwnTicks = 0;
    }
};

}  // namespace edvr::draw_cpu
