// The compositor's own frame timing, read once a frame for the menu's
// Monitor page (docs/settings-menu.md).
//
// fpsVR's numbers -- the app's GPU time, the compositor's, dropped and
// reprojected frames, the CPU frame interval -- are the runtime's own
// measurements, offered to every application through
// IVRCompositor::GetFrameTiming. One call per frame at the WaitGetPoses
// boundary, a copy out of the runtime's shared memory, published whole to
// the d3d11 half over the channel. Nothing is polled, nothing is timed here.
//
// THE SLOT. GetFrameTiming is slot 8 of every IVRCompositor generation this
// build knows (014 through 028): the generations that changed the table
// inserted after it, never before. It is still range-checked against the
// table's executable prefix and guarded, and the first answer is validated
// -- the struct's size echoed back, a finite interval, a frame index that
// moves -- before any later one is believed. A refusal is final for the
// session and says why; the Monitor page then shows what it can measure
// itself.
//
// THE STRUCT. The runtime checks the size field against the struct of the
// generation it is serving, so the 1.0-era 176-byte layout is offered first
// and the 184-byte one (two vsync counts appended) second. Either way only
// the fields both share are read.
#pragma once

#include <cstddef>

namespace edvr {

// Reads advanced.compositor_timing (on | off). Install AND reload.
void frameTimingConfigure();

// Once per frame at the boundary, after the real WaitGetPoses: read and
// publish. `iface` is the compositor the game was handed, `prefix` its
// table's executable prefix.
void frameTimingBoundary(void* iface, size_t prefix);

// The session's line for the shutdown totals.
void frameTimingShutdown();

}  // namespace edvr
