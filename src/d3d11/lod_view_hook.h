#pragma once
// The LoD view probe (docs/elite-ss-hangar-door-2026-10-21.md; the logic and the evidence are in lod_view_core.h, the rig is tools\lod_view_test):
// a CodeHook on the game's scene-view builder (EliteDangerous64.exe+0x2811E40, build 334473), installed when the scope includes flat and acting
// only while the flat profile is active. TEMPORARY: the instrument of the Supersampling hangar door arc. It logs each view's size and the pixel
// size the game derived from it ("lod view:" lines), and advanced.detail_at_output_size (temporary, off by default, live) scales that pixel size
// to the output's for views drawn at the render size. Remove it with the arc, or replace it with the correction it measures.
#include <cstdint>

namespace edvr {

// Install the hook (once; later calls do nothing). Called from device_hook at device creation, after the other startup installers. A build whose
// PE stamp or whose entry's sixteen bytes are not 334473's is one log line and nothing patched, never retried.
void lodViewInstall();

// Once a second from device_hook's config tick: reads advanced.detail_at_output_size (a change is one line), writes the due "lod view:" lines,
// and says so once when 60 s pass with the hook installed and no scene view built.
void lodViewPoll(uint64_t nowMs);

}  // namespace edvr
