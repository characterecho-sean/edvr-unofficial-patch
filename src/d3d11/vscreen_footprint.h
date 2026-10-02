// The footprint instrument: how wide the on-foot screen is in each eye, in eye pixels (docs/design-flat-temporal-aa-2026-09-23.md,
// section 82, the "vscreen auto-fit" entry of 2026-10-01).
//
// fix.vscreen_res_width = auto sizes the on-foot screen's texture from this number when the VR world route will run
// (src/common/vscreen_fit.h has the rule, vscreen_res.cpp applies it at launch). The width is restart-only -- the game
// allocates at startup -- so a launch is the only time it can change, and the number it needs has to come from a SESSION
// BEFORE: this module measures it while the game runs, logs it every 30 s, and stores the on-foot head-on floor (the 10th
// percentile of the session's widths, not their median: a head not square on to the screen only widens it) beside the eye
// width (vscreen_auto_state.cpp) for the next launch.
//
// WHAT IT MEASURES. At the 2D screen's composite draw (vs 5C36AF05 ps CFE84157, two draws a frame, one an eye) the four
// corners of the quad are pushed through the composite vertex shader's own arithmetic (docs/shaders/composite-vs.asm):
// the quad's four vertices and its per-instance SIZE, the model-to-scene rows cb0[9..11], the clip columns cb1[270..273].
// The horizontal NDC extent over two is the fraction of the eye's width the screen spans; times the eye's width in pixels
// it is the screen's width in eye pixels. Geometry rather than an occlusion query because the query counts AREA (a width
// follows only by assuming the screen's shape), is clipped where the screen overflows the eye, and needs a GPU timestamp's
// worth of latency handling; the corners cost nothing on the GPU and clip nothing.
//
// HOW IT READS THEM. Once about every half second, one composite draw's four sources are copied into one 256 byte staging
// buffer (the constant buffers' rows, the first four vertices, the SIZE record), and the copy is mapped three or more frames
// later at the frame boundary with D3D11_MAP_FLAG_DO_NOT_WAIT -- the render thread never waits on the GPU. Every D3D call
// here steps past EDVR's own hooks (FlatComputeInternalScope), the whole thing runs on a fault budget of its own, and a
// source that is not what the measurement assumes (a vertex stride that is not 20, a buffer too small) skips the sample and
// is counted by reason on the 30 s line; nothing is ever guessed.
//
// WHEN IT RUNS. The VR profile, fix.vscreen_res_width = auto, and the composite draw reaching the draw hook. With the key
// explicit, or in the flat profile, it never arms, and says so once.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

namespace detail {
extern bool g_footprintWanted;
// The cadences (the 30 s line's window, the gap between samples, how often the arming condition is re-read), as variables only so
// tools\vscreen_footprint_glue_test can run a 30 s window in milliseconds. The DLL never writes them.
extern uint64_t g_footprintWindowMs;
extern uint64_t g_footprintSampleMs;
extern uint64_t g_footprintConfigMs;
}

// One load for the draw hook: the instrument is armed.
inline bool vscreenFootprintWanted() { return detail::g_footprintWanted; }

// At the 2D screen's composite draw, on the owner context, AFTER the game's own issue (the constants it read are still
// bound). appliedDistance is the panel distance the draw's constants carry: fix.panel_distance when the distance override
// replaced the draw's cb0, 1.0 when the game's own did. baseVertex and startInstance are the draw's own arguments.
void vscreenFootprintCompositeDraw(ID3D11DeviceContext* ctx, float appliedDistance, int baseVertex, unsigned startInstance);

// Once a frame at the Present boundary, on the render thread: reads the arming condition (at most once a second), maps a
// sample whose copy has had time to run, and every 30 s prints the window's line and stores the on-foot p10. onFoot is
// the world-screen gate (the layer's, else the journal's): the screen shows the world.
void vscreenFootprintFrameBoundary(ID3D11DeviceContext* ownerCtx, bool onFoot);

// Releases the staging buffer. From vScreen's shutdown, where the device is still alive.
void vscreenFootprintShutdown();

// A sample's copies are issued and not yet mapped. Read by nothing in the DLL: tools\vscreen_footprint_glue_test waits on it, because a
// WARP device finishes a copy a moment after the context says it is done.
bool vscreenFootprintSamplePending();

}  // namespace edvr
