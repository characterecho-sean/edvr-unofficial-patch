#pragma once
#include <windows.h>
#include <stdint.h>

#define EDVR_NATIVE_PRESENT_TRACE_VERSION_1 1u
constexpr unsigned EDVR_NATIVE_PRESENT_TRACE_CAPACITY = 16;

inline uint64_t edvrNativeTraceUs(int64_t ticks) noexcept {
    static const int64_t frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) ? value.QuadPart : int64_t(0);
    }();
    return ticks > 0 && frequency > 0 ? static_cast<uint64_t>(
        static_cast<double>(ticks) * 1000000.0 / static_cast<double>(frequency)) : 0;
}
inline uint64_t edvrNativeTraceNowUs() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) ? edvrNativeTraceUs(value.QuadPart) : 0;
}

// CPU-only observation of the owned game's Present hook. All marks use QPC
// microseconds, shared with the native caller's frame-cycle clock. No GPU wait
// or query is introduced. The native-timing lease owns the reader's lifetime.
struct EdvrNativePresentSpan {
    uint64_t beginUs, realBeginUs, realEndUs, bodyEndUs, endUs;
    uint32_t thread, syncInterval, flags;
    int32_t result;
};
struct EdvrNativePresentTrace {
    uint32_t size, version, count, overflow;
    uint64_t generation, totalObserved;
    EdvrNativePresentSpan spans[EDVR_NATIVE_PRESENT_TRACE_CAPACITY];
};
static_assert(sizeof(EdvrNativePresentSpan) == 56, "native Present span ABI");
static_assert(sizeof(EdvrNativePresentTrace) == 928, "native Present trace ABI");

extern "C" HRESULT WINAPI edvrReadNativePresentTrace(
    void* timingContext, uint64_t beginUs, uint64_t endUs,
    EdvrNativePresentTrace* trace);
