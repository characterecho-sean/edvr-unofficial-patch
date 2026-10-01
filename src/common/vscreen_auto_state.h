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

#include "vscreen_fit.h"

namespace edvr {

// The last session's resolved per-eye width, or false when none is on record
// yet -- a fresh install, or a log directory that was cleared.
bool lastKnownEyeWidth(const std::wstring& logDir, uint32_t* outWidth);

// Remembers the per-eye render width this session actually settled on, so a
// later launch's "auto" panel size can track it. No-op for a width of 0.
// Called from native_render_settings.cpp once the OpenXR host resolves a
// real size.
void noteResolvedEyeWidthForVScreenAuto(const std::wstring& logDir, uint32_t eyeWidth);

// The second fact "auto" needs once the VR world route runs: how wide the on-foot
// screen is in the eye, measured by the footprint instrument (vscreen_footprint.cpp)
// and stored beside the eye width as a FRACTION of the eye width at panel distance
// 1.0, so a changed panel_distance or eye width rescales it with no new measurement
// (vscreen_fit.h says why). False when none is on record, or the file does not hold a
// plausible fraction.
bool lastKnownPanelFootprint(const std::wstring& logDir, vscreenfit::Record* out);

// Remembers a session's on-foot median. Called from the footprint instrument's 30 s
// line, only with enough samples to be a median (vscreen_footprint.cpp).
void noteMeasuredPanelFootprint(const std::wstring& logDir, const vscreenfit::Record& record);

}  // namespace edvr
