#pragma once
#include "native_timing.h"
#include "gpu_frame_timing.h"
#include <cstdint>

namespace edvr {
struct NativePerfAverage { unsigned count=0; double meanMs=0; };

// The monitor's CPU figure from one runtime frame, by the rule the settlement
// governor's readWork uses: the caller work per cycle when the frame is timing
// version 5 and carries it (EdvrNativeTimingFrame::callerWorkMs with
// callerWorkValid) -- the game's thread from one pose wait's return to the
// next one's entry, submits included: everything the display period must
// hold. From an older runtime (version 3 or 4, no caller work) the producer's
// application time stands in (NativeTimingSnapshot::applicationMs: pose wait
// end to submit plus the eye treatments, the pre-submit phase only; on the
// 2026-09-23 flights it read under 10 ms while the caller thread worked
// 11.7-12.7 ms a cycle). A version 5 frame without valid caller work has no
// figure -- never the application time beside it -- so no average mixes them.
enum class NativeCpuSource : unsigned char { None, CallerWork, PreSubmit };
struct NativeCpuFigure { NativeCpuSource source=NativeCpuSource::None; bool valid=false; double ms=0; };

// Menu producer only. Observations are independent completed samples, never
// assigned to the Present frame that happens to read an asynchronous result.
class NativePerfHistory final {
public:
    void observe(bool native, const NativeTimingSnapshot&, const GpuFrameSnapshot&, uint64_t nowMs) noexcept;
    void clear() noexcept;
    NativePerfAverage submit(uint64_t nowMs, uint64_t windowMs) const noexcept;
    // The values suitable for the UI, independent from the submit wall/XR wait
    // diagnostics: applicationCpu is the CPU figure above (cpuFigure) and the
    // CPU graph plots it; applicationGpu is the application render's elapsed
    // GPU time.
    NativePerfAverage applicationCpu(uint64_t nowMs, uint64_t windowMs) const noexcept;
    NativePerfAverage applicationGpu(uint64_t nowMs, uint64_t windowMs) const noexcept;
    static NativeCpuFigure cpuFigure(const NativeTimingSnapshot&) noexcept;
    // Which figure applicationCpu and the CPU graph hold, for the label:
    // CallerWork, PreSubmit (an older runtime), or None before the first.
    NativeCpuSource cpuSource() const noexcept { return cpuSource_; }
    NativePerfAverage wait(uint64_t nowMs, uint64_t windowMs) const noexcept;
    NativePerfAverage producer(uint64_t nowMs, uint64_t windowMs) const noexcept;
    NativePerfAverage transfer(uint64_t nowMs, uint64_t windowMs) const noexcept;
    NativePerfAverage compose(uint64_t nowMs, uint64_t windowMs) const noexcept;
    double predictedPeriod(uint64_t nowMs) const noexcept;
    // The base display period in milliseconds that predictedPeriod is compared
    // against: the runtime's published display frequency while fresh (the ABI's
    // baseDisplayHz), else the session's first predicted period. 0 when neither
    // is known. Survives ring staleness only through the first-period fallback.
    double basePeriodMs(uint64_t nowMs) const noexcept;
    // The display-rate throttle test: the runtime predicting more than 1.5x the
    // base period means the headset is running at a throttled display rate
    // (e.g. SteamVR halving 90 Hz to 45 Hz under load), whatever the producer
    // cadence reads.
    static bool displayThrottled(double predictedPeriodMs, double basePeriodMs) noexcept;
    int graph(bool producer, float* out, int max, uint64_t nowMs) const noexcept;
private:
    static constexpr unsigned kCapacity=900;
    struct Entry { uint64_t at=0; double values[2]{}; };
    struct Stream {
        Entry entries[kCapacity]{};
        unsigned count=0, next=0;
        uint64_t floor=0;
        void reset(uint64_t consumed=0) noexcept;
        void add(uint64_t sequence, uint64_t at, double a, double b=0) noexcept;
        const Entry& at(unsigned index) const noexcept;
        bool fresh(uint64_t now) const noexcept;
        NativePerfAverage average(unsigned field,uint64_t now,uint64_t window) const noexcept;
        int graph(float* out,int max,uint64_t now) const noexcept;
    } cpu_, applicationCpu_, producer_, applicationGpu_, device_;
    uint64_t generation_=0, firstSequence_=0;
    NativeCpuSource cpuSource_=NativeCpuSource::None;
    double period_=0;
    uint64_t periodAt_=0;
    double baseHz_=0;
    uint64_t baseHzAt_=0;
    double firstPeriodMs_=0;
    static bool age(uint64_t now,uint64_t at,uint64_t window) noexcept;
    static bool duration(double value) noexcept;
};
}
