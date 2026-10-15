#pragma once
// The GPU pass profiler (TEMPORARY, 2026-10-12): where Elite's own GPU time goes inside one frame. A measurement, not a fix.
//
// Press hotkey.gpu_pass_profile (default unbound; its value is "KEY" or "KEY COUNT", COUNT 1..120, default 30 frames). The
// next COUNT WHOLE frames are measured: each frame gets its own
// frequency scope (gpu_timing.h), and a TIMESTAMP query is issued at every pass boundary on the owner context: an OM
// render-target change, a clear, a compute dispatch, and an EDVR census section's begin and end. The time between two
// consecutive stamps is one segment, attributed to the targets bound when it began. Segments inside EDVR census sections
// are labelled edvr and excluded from Elite's total. Output: edvr_logs\gpu_pass_profile_<stamp>.csv (one row per segment
// per frame) and "gpu pass profile:" summary lines. Unarmed, every hook is one bool read.
//
// Calls come from the thunks in vscreen.cpp, exposure_fix.cpp (dispatch) and gpu_census.cpp (EDVR sections), and from
// vScreenFrameBoundary once per Present. Nothing here issues a draw or changes a binding.
#include <cstdint>
#include <string>

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

namespace edvr {

namespace detail {
extern bool g_gpuPassCapturing;   // true while a frame is open on the owner context
}
// The one bool read every hook makes before anything else.
inline bool gpuPassProfileCapturing() { return detail::g_gpuPassCapturing; }

// The hotkey. Arms the next capture; a press while armed or running is refused with a note. The frame count is read from the
// hotkey.gpu_pass_profile value at the press.
void gpuPassProfileRequest();
// The key part of a hotkey.gpu_pass_profile value: the value without a trailing frame count (what the hotkey binds).
std::string gpuPassProfileKeyPart(const std::string& value);

// OMSetRenderTargets (and the UAV variant's render-target half): the targets now bound. Ignored unless ctx is the owner.
void gpuPassProfileNoteOm(ID3D11DeviceContext* ctx, uint32_t n, ID3D11RenderTargetView* const* rtvs,
                          ID3D11DepthStencilView* dsv);
// ClearRenderTargetView (rtv set) or ClearDepthStencilView (dsv set).
void gpuPassProfileNoteClear(ID3D11DeviceContext* ctx, void* rtv, void* dsv);
// One draw. kind as draw_census: 'D' Draw, 'I' DrawIndexed, 'N' DrawInstanced, 'X' DrawIndexedInstanced, others 0 triangles.
void gpuPassProfileNoteDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances);
void gpuPassProfileNoteDispatch(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z);
// An EDVR census section (not one of the Altered sections) begins or ends.
void gpuPassProfileNoteEdvr(ID3D11DeviceContext* ctx, bool enter);

// Once per Present, from vScreenFrameBoundary: closes the frame in progress, starts the next one of a batch, reads the
// results of finished frames, and writes the CSV and summary once every frame of a batch is resolved.
void gpuPassProfileFrameBoundary(ID3D11DeviceContext* ctx, uint32_t frameNo, uint32_t eyeW, uint32_t eyeH);

// Quiescent cleanup: releases the query pool. Called where the census shuts down.
void gpuPassProfileShutdown();

}  // namespace edvr
