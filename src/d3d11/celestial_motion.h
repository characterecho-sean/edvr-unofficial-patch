#pragma once

#include <d3d11.h>
#include <cstdint>

namespace edvr {
// Terrain uses a view-space quaternion/translation outside the instance
// pool. Preserve those draw-time transforms on the GPU and rasterise an
// index into a private layer. The temporal pass accepts it only where its
// depth agrees with the final scene, never over foreground UI or ships.
void celestialMotionConfigure(bool enabled);
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
