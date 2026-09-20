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
unsigned nativeTimingReadCompletions(uint64_t& cursor, NativeTimingSnapshot* out,
                                     unsigned capacity, uint64_t& dropped) noexcept;
// CPU-only producer hook observation. Records only the device owned by the
// current native timing lease; no COM ownership or graphics work is added.
uint64_t nativeTimingPresentBegin(ID3D11Device*, uint64_t beginUs,
                                  uint32_t thread) noexcept;
void nativeTimingNotePresent(ID3D11Device*, uint64_t token,
                             const EdvrNativePresentSpan&) noexcept;
}
