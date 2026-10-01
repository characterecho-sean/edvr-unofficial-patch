#pragma once
#include "../common/native_timing.h"
#include "../common/native_present_trace.h"

namespace edvr {
struct NativeTimingSnapshot {
    bool active=false, haveCpu=false, invalid=false;
    uint64_t generation=0, firstSequence=0, sequence=0, capturedAtMs=0;
    double waitMs=0, predictedPeriodMs=0;
    // Sum of producer wall intervals bracketed from the end of pose wait to
    // submit admission and around each native treatment callback. This is
    // elapsed wall time, including descheduling and game stalls; it is not
    // exclusive CPU execution time.
    double applicationMs=0;
    bool applicationValid=false;
    EdvrNativeTimingFrame cpu{};
    bool haveDeviceGpu=false;
    EdvrNativeDeviceGpuSample deviceGpu{};
};
// CPU snapshot; consumers reject samples older than 2 seconds. GPU data comes
// from gpuFrameSnapshot(), is separately aged, and must belong to this session
// (sequence >= firstSequence, with nonzero firstSequence). Never add device
// GPU spans together or present either span as compositor GPU time.
NativeTimingSnapshot nativeTimingSnapshot() noexcept;
// The two most recent pose-wait returns of the current timing context, on the QPC clock the Present hook
// reads. The runtime's frame cycle runs from one WaitGetPoses return to the next
// (native_frame_cycle_window's boundary=host_wait_return_to_next_host_wait_return), so these two stamps give
// the graphics half the same cycle the runtime logs a native_long_cycle line for, as it happens and not a
// cycle late: the previous cycle whole (returnQpc - previousQpc) and the current one up to now
// (now - returnQpc). That is how the monitor tells a one-frame Present-gap blip, which the runtime's cycle
// does not show, from a real stall (perf_monitor.cpp). valid is false until two waits have returned, and
// whenever no timing context is open.
struct NativeWaitReturns {
    bool valid = false;
    uint64_t sequence = 0;     // the wait that returned last (the same counter as the LONG FRAME line's runtime sequence)
    int64_t returnQpc = 0;     // when it returned
    int64_t previousQpc = 0;   // when the wait before it returned
};
NativeWaitReturns nativeTimingWaitReturns() noexcept;
// Called once from the timing table's close, after the context is released and the lock is dropped: the
// session is over, so the monitor writes its end-of-session lines (perf_monitor.cpp). Null in a rig, which
// is how native_timing.cpp links without perf_monitor.cpp.
inline void (*g_nativeTimingCloseObserver)() = nullptr;
unsigned nativeTimingReadCompletions(uint64_t& cursor, NativeTimingSnapshot* out,
                                     unsigned capacity, uint64_t& dropped) noexcept;
// CPU-only producer hook observation. Records only the device owned by the
// current native timing lease; no COM ownership or graphics work is added.
uint64_t nativeTimingPresentBegin(ID3D11Device*, uint64_t beginUs,
                                  uint32_t thread) noexcept;
void nativeTimingNotePresent(ID3D11Device*, uint64_t token,
                             const EdvrNativePresentSpan&) noexcept;
}
