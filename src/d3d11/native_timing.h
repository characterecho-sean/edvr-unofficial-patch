#pragma once
#include "../common/native_timing.h"

namespace edvr {
struct NativeTimingSnapshot {
    bool active=false, haveCpu=false, invalid=false;
    uint64_t generation=0, firstSequence=0, sequence=0, capturedAtMs=0;
    double waitMs=0, predictedPeriodMs=0;
    EdvrNativeTimingFrame cpu{};
};
// CPU snapshot; consumers reject samples older than 2 seconds. GPU data comes
// from gpuFrameSnapshot(), is separately aged, and must belong to this session
// (sequence >= firstSequence, with nonzero firstSequence). Never add device
// GPU spans together or present either span as compositor GPU time.
NativeTimingSnapshot nativeTimingSnapshot() noexcept;
}
