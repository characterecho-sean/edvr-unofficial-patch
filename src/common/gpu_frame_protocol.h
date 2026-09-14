#pragma once
#include <cstdint>

namespace edvr {
// Paired-proxy CPU messages. Wait events never issue D3D commands. Sequence
// numbers are allocated by the graphics receiver and survive compositor rebind.
constexpr unsigned kGpuFrameProtocol = 1;
enum class GpuFrameEvent : unsigned {
    WaitBegin, WaitEnd, SubmitBegin, SubmitEnd, Cancel
};
// WaitBegin returns a new sequence. WaitEnd arms that sequence when flags == 1.
// SubmitBegin accepts an ID3D11Texture2D* and returns 1 only if its interval
// began on the bound immediate-context thread. SubmitEnd flags == 1 means an
// actual runtime Submit was forwarded and accepted; 0 invalidates the frame.
// The submit interval includes runtime Submit and any post-submit EDVR copies.
// Cancel invalidates the specified sequence. All other returns are 0/1.
} // namespace edvr
