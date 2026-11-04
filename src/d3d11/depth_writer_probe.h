// "Who wrote this pixel's depth" -- the per-pixel answer the pixel probe cannot give. During ONE armed eye-dump
// frame (the same seam and the same key as pixel_probe.h: advanced.pixel_probe set to any point arms both), every
// draw that can write an eye's scene depth is followed by a copy of that depth and a compute pass that stamps the
// draw's id into a per-eye R32_UINT map wherever the depth differs from the copy before it. At the frame's close
// the map is read back once and written beside the object ledger:
//
//   edvr_logs\pool\depthwriter_<stamp>_e<eye>.bin   EDVRTEX1, DXGI 42 (R32_UINT), the id of the last draw that
//                                                    changed each pixel's depth, 0 = no draw did (the clear)
//   edvr_logs\pool\depthwriter_<stamp>.json          one row per id: the draw's shaders, counts and the
//                                                    depth, stencil, blend and rasteriser state it ran under
//
// and the log names the draws that own the most pixels. The reason it exists: stale-slot pixels (the engine motion
// compose finds a pool record's slot at a pixel whose depth is not the slot's) carry no history, and which draw
// wrote the depth that disowned them was not in any capture (docs/kinematic-motion-injection-2026-09-19.md, "ship
// external camera"). Joined to the same run's EngineSlots and Z files (tools/depth_writer_join.py) the map names it.
//
// Cost: one depth copy and one full-eye compute pass per depth-writing draw of the armed frame (about 0.3 ms each
// on the pilot's card, a few hundred draws: a hitch of the eye dump's own order). Nothing runs outside that frame
// but the one bool load depthWriterWantsDraws() is for.
//
// Narrow on purpose, like pixel_probe.h: it asks the caller for nothing but the context, the draw's DrawInfo and a
// resolver that says which eye a depth view is (vscreen.cpp passes the depth probe's), so tools/depth_writer_probe_test
// drives it on WARP without the rest of the DLL.
#pragma once

#include <cstdint>

#include "pixel_probe.h"   // DrawInfo

struct ID3D11DeviceContext;
struct ID3D11DepthStencilView;

namespace edvr {

// The eye (0 or 1) whose scene depth `dsv` is, false for any other view. Set once at start-up.
using DepthWriterEyeResolver = bool (*)(ID3D11DepthStencilView* dsv, int* outEye);
void depthWriterSetEyeResolver(DepthWriterEyeResolver resolver);
// Where the files go. Unset (the DLL), edvr_logs\pool beside the object ledger; a rig names its own folder.
void depthWriterSetOutputDir(const wchar_t* dir);

namespace detail {
extern bool g_depthWriterActive;
}
// True only inside the armed frame, so an idle probe costs the hooks this one load.
inline bool depthWriterWantsDraws() { return detail::g_depthWriterActive; }

// Arms the probe for the whole frame after the next boundary (the pixelProbeArm timing). `stamp` is the eye run's,
// for the file names. A run already armed is left alone. The caller decides whether the probe is wanted
// (temporal_pass.cpp: only when advanced.pixel_probe has a point).
void depthWriterArm(const wchar_t* stamp);
bool depthWriterArmed();

// The frame edge, called every frame beside pixelProbeFrameBoundary: closes and reports the probed frame once it
// has fully elapsed (read the maps back, write the files, log, release); cheap when unarmed.
void depthWriterFrameBoundary(ID3D11DeviceContext* ctx);

// Before a hooked draw: takes the baseline copy the first time each eye's scene depth is bound in the probed frame.
void depthWriterBeforeDraw(ID3D11DeviceContext* ctx);

// After a hooked draw (and any EDVR reissues for it) has fully returned. A draw that cannot write depth (depth off,
// write mask zero) or is not into an eye's scene depth is not followed.
void depthWriterAfterDraw(ID3D11DeviceContext* ctx, const DrawInfo& info);

void depthWriterShutdown();

}  // namespace edvr
