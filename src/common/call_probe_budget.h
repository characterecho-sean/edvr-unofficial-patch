#pragma once
#include <atomic>
#include <cstdint>

namespace edvr {
// Temporary LOD/exit diagnostic. Admission never waits; four early samples,
// then one per twenty seconds, sixteen total. No frame count stands in for time.
// Keep this header platform-independent: Windows headers define near/far macros.
class GameCallProbeBudget {
    std::atomic_flag lock_=ATOMIC_FLAG_INIT;
    unsigned samples_=0;
    uint64_t last_=0;
public:
    unsigned take(uint64_t now) noexcept {
        if(lock_.test_and_set(std::memory_order_acquire))return 0;
        unsigned sample=0;
        if(samples_<16 && (samples_<4 || (now>=last_ && now-last_>=20000))) {
            sample=++samples_;last_=now;
        }
        lock_.clear(std::memory_order_release);return sample;
    }
};
}
