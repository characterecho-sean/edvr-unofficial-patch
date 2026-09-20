// The one fact fix.vscreen_res_width = auto needs that it cannot get from the
// running session: the per-eye render width the OpenXR runtime last resolved.
//
// That width is not known until well after the on-foot panel patch has to
// run (vscreen_res.cpp applies it at D3D11 device creation; the runtime host
// that resolves a real per-eye size is not even constructed until the game's
// device has Presented). So "auto" is computed from what got written here at
// the END of the previous session, not a live read.
//
// Deliberately free of anything vscreen- or D3D11-specific -- this is Win32
// file I/O only -- so it links into native_render_settings.cpp's minimal
// test rigs (native_frame_test, native_render_settings_test) without pulling
// in the rest of d3d11.dll the way vscreen_res.cpp would.
#pragma once

#include <cstdint>
#include <string>

namespace edvr {

// The last session's resolved per-eye width, or false when none is on record
// yet -- a fresh install, or a log directory that was cleared.
bool lastKnownEyeWidth(const std::wstring& logDir, uint32_t* outWidth);

// Remembers the per-eye render width this session actually settled on, so a
// later launch's "auto" panel size can track it. No-op for a width of 0.
// Called from native_render_settings.cpp once the OpenXR host resolves a
// real size.
void noteResolvedEyeWidthForVScreenAuto(const std::wstring& logDir, uint32_t eyeWidth);

}  // namespace edvr
