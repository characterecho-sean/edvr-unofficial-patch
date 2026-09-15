#pragma once
#include <cstdint>

namespace edvr {
// Paired-proxy CPU messages. Wait events never issue D3D commands. Sequence
// numbers are allocated by the graphics receiver and survive compositor rebind.
constexpr unsigned kGpuFrameProtocol = 1;
constexpr unsigned kGpuFrameNativeWait = 3; // valid wait plus segmented native measurement
enum class GpuFrameEvent : unsigned {
    WaitBegin, WaitEnd, SubmitBegin, SubmitEnd, Cancel, SegmentResume,
    SegmentPause, SegmentEnd
};
// WaitBegin returns a new sequence. WaitEnd flags 1 retains legacy outer timing;
// kGpuFrameNativeWait selects explicitly admitted native rendering segments.
// SubmitBegin accepts an ID3D11Texture2D* and returns 1 only if its interval
// began on the bound immediate-context thread. SubmitEnd flags == 1 means an
// actual runtime Submit was forwarded and accepted; 0 invalidates the frame.
// SegmentEnd closes the physical eye-treatment interval before transfer.
// SubmitEnd only records acceptance and closes the shared query scope after
// the second eye; it never moves a rendering timestamp past runtime work.
// SegmentResume admits the next real producer command after a route return;
// SegmentPause ends game work before entering the next submit route.
// Cancel invalidates the specified sequence. All other returns are 0/1.
} // namespace edvr
