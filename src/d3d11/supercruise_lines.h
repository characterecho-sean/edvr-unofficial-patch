// The three supercruise draws fix.ui_quality takes into the HDR layer (docs/ui-layer-2026-09-23.md, "2026-10-07: orbit
// lines, supercruise bars and space dust in the layer"): their shader hashes, in one place, for the pure family rule
// (ui_layer_math.h), the draw hook (vscreen.cpp), the aux capture (object_probe.cpp) and the rigs.
//
//   ORBIT LINES: vs C7FA0C0F5DD49180 / ps 6EEF165A350DA30F. A triangle strip of 8194 vertices, a handful of instances
//   (the orange orbits, half-width 2.0 px of the render target; the cyan ring lines 1.5), drawn into the lit HDR scene
//   target with depth GEQUAL (no write) and stencil EQUAL (reference 1, read mask 0x81, no write). orbital_width.h holds
//   their width patch; the hashes there are the same numbers (static_assert in orbital_width.h).
//
//   SUPERCRUISE BARS: vs A47A3315FFF5E2E4 / ps 869FFF43E875906E. A line list of 140 vertices (70 segments), one
//   instance, vertex stride 32 (POSITION, NORMAL), into the same HDR target with depth and stencil off and the blend
//   SRC_ALPHA / INV_SRC_ALPHA. The vertex shader multiplies NORMAL by (5, 5, 5, 1) for the colour and transforms the
//   camera-relative POSITION with three rows of cb0 (x, y and w; z is 0); the pixel shader scales the colour by
//   cb1[90].y and takes alpha from the vertex. They are 1 px lines, with no width term. The vertex shader is seen with a
//   second pixel shader in the flat recipes (flat_projection_recipes.h): the pair is the key here, never the vertex
//   shader alone. WHAT THEY ARE is evidence, not yet proof: 1 px dim-yellow lines, no depth, the only line-topology
//   draw of the frame, drawn beside the supercruise HUD. The identity is proved by the next eye dump's vertex capture
//   (object_probe.cpp's aux capture now records this draw's 70 segments).
//
//   SPACE DUST: vs 9BFC7FD232328391 / ps DBF1725726018F52 ONLY (the vertex shader is paired with ps CB7AF179 for the flat route,
//   flat_projection_recipes.h: that pair stays out). DrawInstanced of 1800 vertices, one instance, a triangle list of 300 ribbon quads
//   (the white particles that stream toward the ship in supercruise), into the same lit HDR scene target. Additive (ONE, ONE), depth
//   GEQUAL with no write, stencil ALWAYS with REPLACE on all three outcomes under mask 0x40 (reference 8 or 4): a write that clears a
//   bit no SceneZ stencil value has ever carried, which the layer keeps all the same (it re-issues a taken draw's stencil write
//   colourless into the game's own buffer). The vertex shader builds each ribbon's width itself (v1.y x cb2[0].y x cb2[8].x across the
//   streak, in world units), and the pixel shader reads no position, so the draw is taken with no change of shader. Evidence, from the
//   census of 2026-10-07 05:48 and the dump 054804: a streak moves about 51 render pixels a frame against its own 22, so successive
//   streaks never overlap and the upscaler has nothing to accumulate (it keeps 0.69 of the energy, the stars 0.96).
#pragma once

#include <cstdint>

namespace edvr {

constexpr uint64_t kUiVsOrbitLines = 0xC7FA0C0F5DD49180ull;
constexpr uint64_t kUiPsOrbitLines = 0x6EEF165A350DA30Full;
constexpr uint64_t kUiVsSupercruiseBars = 0xA47A3315FFF5E2E4ull;
constexpr uint64_t kUiPsSupercruiseBars = 0x869FFF43E875906Eull;
constexpr uint64_t kUiVsSpaceDust = 0x9BFC7FD232328391ull;
constexpr uint64_t kUiPsSpaceDust = 0xDBF1725726018F52ull;

}  // namespace edvr
