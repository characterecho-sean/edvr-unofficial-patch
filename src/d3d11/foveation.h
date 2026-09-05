// Foveated shading -- docs/performance.md, feature 2 (fixed centre first).
//
// WHAT IT IS
//
// The game is asked to shade the edges of each eye coarsely: one pixel
// shader run per 2x2 block in a ring around the fovea, one per 4x4 beyond
// it, full rate inside. Rasterisation, depth and edges stay per-pixel
// everywhere -- only the shading is coarse -- so the periphery reads as a
// softer surface, never a jagged one, and DLSS or DLAA then runs over the
// whole frame at submit exactly as before, with one continuous history.
// The saving is the game's own pixel-shading cost, which in a deferred
// renderer is most of the frame, and that saving is what buys render
// resolution back (docs/performance.md explains the arithmetic).
//
// HOW
//
// D3D11 reaches variable-rate shading only through NvAPI (NVIDIA Turing
// and later). A shading-rate image -- one byte per 16x16-pixel tile,
// R8_UINT -- is made per eye-texture size and per eye, filled on the CPU
// from each eye's frustum tangents (the straight-ahead point sits off the
// texture centre in an asymmetric frustum) shifted toward the nose by the
// eye's offset over a fixation distance, and bound with the per-viewport
// rate table whenever the game draws into an eye-sized target. Any other
// target, ClearState and the frame boundary unbind it. The NvAPI entry
// points are resolved by their published IDs (nvapi_interface.h, MIT) and
// the three structures transcribed from NVIDIA's reference documentation;
// nothing NVIDIA ships is vendored.
//
// Which eye a target belongs to: the texture the openvr half submitted for
// that eye when the target IS one (frame_flag.h), else the depth probe's
// rule -- of two targets alike in size and format, the first bound in the
// frame is the left eye's. A wrong guess moves that target's fovea by the
// nasal shift and the frustum's asymmetry, a few degrees.
//
// FAIL-SAFES
//
// Arming needs nvapi64.dll, NvAPI_Initialize, the capability bit, the view
// to create and the first set call to answer OK; anything else is one log
// line and off for the session. Pimax Play's own foveated rendering holds
// the same shading-rate image (LibMagicD3D1164.dll); when that module is
// loaded this stands down and says so. Every NvAPI call runs under the
// module's fault budget. Off, this costs one bool per draw.
//
// SETTINGS
//
//   fix.foveation = off | quality | balanced | performance
//   advanced.foveation_inner, _outer (degrees across; 0 = the preset's),
//   advanced.foveation_distance (metres; the nasal shift),
//   advanced.foveation_passes = all | geometry
//
// All live: a change regenerates the images at their next use.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

void foveationConfigure(Config& cfg);

// One bool for the draw path's early-out set.
bool foveationWantsDraws();

// Every draw on the owner context: whether slot 0's target is eye-sized
// (the census's own verdict), the view and its binding generation, and the
// draw's shape for the passes filter. Sets or clears the shading-rate image
// when the answer changes; a compare otherwise.
void foveationOnDraw(ID3D11DeviceContext* ctx, bool rtvEyeSized, void* rtv, uint32_t rtvGen,
                     char kind, uint32_t count, uint32_t instances);

// Once per frame with the owner context: unbinds the image, resets the
// per-frame eye attribution, ages the images out, prints the summaries.
void foveationFrameBoundary(ID3D11DeviceContext* ctx);

// From the ClearState hook: whatever was bound may be gone.
void foveationOnClearState();

void foveationShutdown();

}  // namespace edvr

extern "C" {
// The desk test (tools/smoke): on the given device, arm NvAPI, draw a
// 512x512 target through a shading-rate image whose tile rows cycle through
// every rate the table can name, and measure the shaded block size per row
// from the readback. Returns 1 when 1x1, 2x2 and 4x4 measure as themselves,
// 2 when the image had an effect but a rate did not measure as named, 0
// when it had no effect at all, -1 when it could not run here (no NVIDIA
// driver, no support). `report` receives the lines.
__declspec(dllexport) int edvrFoveationProbe(void* device, void* context, char* report,
                                             unsigned reportBytes);
}
