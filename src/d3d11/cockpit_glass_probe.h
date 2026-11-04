#pragma once
// TEMPORARY instrument (experimental.cockpit_glass_probe; docs\cockpit-glass-legacy-port.md section 5). Off by default; remove it when
// the legacy-glass port is written.
//
// The legacy cockpit glass port replaces the live canopy's pixel shader (CockpitGlass_Forward) with one that reads what the game has
// already bound. What it reads is known by register and slot from the effects archive, but not the values, the draw's output state or
// the light buffer's layout. With the key on, the first canopy draw and one every 30 s after it (at most 12 a session) log:
//   draw   the draw call, the canopy VS and PS bound and which state and quality they are;
//   om     blend (targets 0 and 1), depth-stencil, rasterizer, the render targets and depth target;
//   srv    PS t0..t11: view dimension, format, size, and a buffer view's first element and count;
//   smp    PS s0..s2;
//   cb     PS b1 and b2: size, usage, and the bound range when the context reports one;
// and, a few frames later at the frame boundary (the GPU copies made at the draw read back with DO_NOT_WAIT, never a stall):
//   cb1    the frame block registers the port reads (cockpit_glass_probe_core.h kCb1Spans);
//   cb2    the material block, all 18 registers;
//   t1     the first 192 bytes of the directional-light buffer.
// Every 30 s a window line counts canopy draws by PS and VS, and draws whose VS is the canopy's but whose PS is none of the nine.
// Armed with no canopy draw, the window line says draws=0 and no draw line follows: "the probe never ran" cannot read as success.
//
// Both profiles. Render thread, owner context only. The draw-path test is one load while off.
#include <d3d11.h>

namespace edvr {

namespace detail {
extern bool g_cockpitGlassProbeLive;
}  // namespace detail

inline bool cockpitGlassProbeLive() { return detail::g_cockpitGlassProbeLive; }

// Before the game's own issue of a draw on the owner context, while cockpitGlassProbeLive(). kind is the census letter
// (D, I, N, X); count and instances as the draw call has them.
void cockpitGlassProbeDraw(ID3D11DeviceContext* ctx, char kind, UINT count, UINT instances);

// Once a frame from vScreenFrameBoundary: reads the key (live), finishes a pending readback, prints the window line.
void cockpitGlassProbeFrameBoundary(ID3D11DeviceContext* ctx);

}  // namespace edvr
