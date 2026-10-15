#pragma once
// The Map timer's sampling (map_wait.h, 2026-10-13): one Map in kMapWaitSampleRate is bracketed with two
// QueryPerformanceCounter reads. The call counts stay exact (every armed Map is counted); the ticks and the
// slow-call count are the sampled ones, scaled by the rate when the totals are taken. Pure, so the rig can test it.
#include <cstdint>

namespace edvr {

constexpr uint32_t kMapWaitSampleRate = 16;  // a power of two: the test is a mask
static_assert((kMapWaitSampleRate & (kMapWaitSampleRate - 1u)) == 0u, "the sample rate is a power of two");

// The per-thread decision. seq is the caller's thread-local counter, starting at zero; true on the
// kMapWaitSampleRate-th call of each run of that many (the 16th, 32nd, ... on a thread).
inline bool mapWaitSampleNext(uint32_t& seq) {
    return (++seq & (kMapWaitSampleRate - 1u)) == 0u;
}

// A total over the sampled Maps, estimated over all of them.
inline uint64_t mapWaitScaleSampled(uint64_t sampled) {
    return sampled * static_cast<uint64_t>(kMapWaitSampleRate);
}

}  // namespace edvr
