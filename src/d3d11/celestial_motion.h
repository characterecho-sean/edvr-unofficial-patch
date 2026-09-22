#pragma once

#include <d3d11.h>
#include <cstdint>

namespace edvr {
// Terrain uses a view-space quaternion/translation outside the instance
// pool. Preserve those draw-time transforms on the GPU and rasterise an
// index into a private layer. The temporal pass accepts it only where its
// depth agrees with the final scene, never over foreground UI or ships.
void celestialMotionConfigure(bool enabled);

// Could a Begin below possibly say yes? A NECESSARY condition only -- Begin
// re-tests this and everything else, so a false here is exactly a Begin that
// would have returned false.
//
// It exists so the draw path can decline without a call. Both Begins are
// invoked once per eye-pass draw, and with fix.temporal_aa off (which is what
// switches this whole feature off, temporal_pass.cpp) they do nothing but
// return -- yet the build is /O2 with no /GL, so "nothing but return" is a
// call, a prologue and an epilogue about 36k times a frame. 249 innermost
// samples of the 1349-frame window of 2026-09-22 landed on this function's
// epilogue, 0.18 ms a frame, spent entirely on declining.
namespace detail {
extern bool g_celestialMotionEnabled;
extern bool g_celestialMotionFailed;
}  // namespace detail
inline bool celestialMotionLive() {
    return detail::g_celestialMotionEnabled && !detail::g_celestialMotionFailed;
}

bool celestialMotionBegin(ID3D11DeviceContext* ctx, uint64_t vs);
// Null-PS terrain prepasses can record coverage in their original draw.
// True requires End immediately after that draw; false leaves it untouched.
bool celestialMotionBeginOriginal(ID3D11DeviceContext* ctx, uint64_t vs);
void celestialMotionEnd(ID3D11DeviceContext* ctx);
void celestialMotionFrameBoundary(ID3D11DeviceContext* ctx=nullptr);
void celestialMotionShutdown();
// CPU-side capture of the terrain constants (the write tees in vscreen.cpp).
// The game writes VS b0/b1/b2 of the terrain draw from the CPU; capturing
// the bytes at the write lets begin() skip the GPU CopySubresourceRegion
// for a slot whose shadow is known current.
void celestialMotionConstantsMapped(ID3D11Resource* resource, void* data);
void celestialMotionConstantsUnmapped(ID3D11Resource* resource);
void celestialMotionConstantsWritten(ID3D11Resource* resource, const void* data, const D3D11_BOX* box);
// A write EDVR cannot see the bytes of: a CopyResource/CopySubresourceRegion
// destination, or nullptr for an executed command list (invalidates all three).
void celestialMotionConstantsUnknownWrite(ID3D11Resource* resource);
// Cumulative census for the regression rig: slots captured from the CPU
// shadow, slots copied on the GPU, and slot re-watches (any counter pointer
// may be null).
void celestialMotionConstantsCensus(unsigned* cpuSlots, unsigned* gpuSlots, unsigned* rewatches);
// Borrowed views: index, depth, motion records. Null on a missing eye/frame.
// Builds the eye's still-pending records first (one batched dispatch that
// covers every draw since the last build), so the motion-record view is
// always current with what has been drawn so far this frame.
void celestialMotionViews(ID3D11DeviceContext* ctx, ID3D11Texture2D* scene, ID3D11ShaderResourceView** views);
// Explicit eye dump only: snapshot the current records, then write them
// after the eye-run grace period. No normal-play readback or disk writes.
void celestialMotionStageDump(ID3D11DeviceContext* ctx, ID3D11Texture2D* scene);
void celestialMotionWriteDump(ID3D11DeviceContext* ctx, const wchar_t* directory, const wchar_t* stamp);
} // namespace edvr
