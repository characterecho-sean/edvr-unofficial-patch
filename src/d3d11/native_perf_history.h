#pragma once
#include "native_timing.h"
#include "gpu_frame_timing.h"
#include <cstdint>

namespace edvr {
struct NativePerfAverage { unsigned count=0; double meanMs=0; };

// Menu producer only. Observations are independent completed samples, never
// assigned to the Present frame that happens to read an asynchronous result.
class NativePerfHistory final {
public:
    void observe(bool native, const NativeTimingSnapshot&, const GpuFrameSnapshot&, uint64_t nowMs) noexcept;
    void clear() noexcept;
    NativePerfAverage submit(uint64_t nowMs, uint64_t windowMs) const noexcept;
    NativePerfAverage wait(uint64_t nowMs, uint64_t windowMs) const noexcept;
    NativePerfAverage producer(uint64_t nowMs, uint64_t windowMs) const noexcept;
    NativePerfAverage transfer(uint64_t nowMs, uint64_t windowMs) const noexcept;
    NativePerfAverage compose(uint64_t nowMs, uint64_t windowMs) const noexcept;
    double predictedPeriod(uint64_t nowMs) const noexcept;
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
    } cpu_, producer_, device_;
    uint64_t generation_=0, firstSequence_=0;
    double period_=0;
    uint64_t periodAt_=0;
    static bool age(uint64_t now,uint64_t at,uint64_t window) noexcept;
    static bool duration(double value) noexcept;
};
}
