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
void celestialMotionEnd(ID3D11DeviceContext* ctx);
void celestialMotionFrameBoundary(ID3D11DeviceContext* ctx=nullptr);
void celestialMotionShutdown();
// Borrowed views: index, depth, motion records. Null on a missing eye/frame.
void celestialMotionViews(ID3D11Texture2D* scene, ID3D11ShaderResourceView** views);
// Explicit eye dump only: snapshot the current records, then write them
// after the eye-run grace period. No normal-play readback or disk writes.
void celestialMotionStageDump(ID3D11DeviceContext* ctx, ID3D11Texture2D* scene);
void celestialMotionWriteDump(ID3D11DeviceContext* ctx, const wchar_t* directory, const wchar_t* stamp);
} // namespace edvr
