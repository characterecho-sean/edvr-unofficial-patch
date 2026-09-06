// The interface writes its depth, so the temporal pass can follow it.
//
// THE DEFECT (Sean, 2026-09-05; measured on five censuses, 2026-09-03 and
// 2026-09-06)
//
// Under fix.temporal_aa every piece of Elite's 2D interface flickers and
// swims: the cockpit's holo panels and the flight HUD above all. Elite
// draws them depth-TESTED and depth-SILENT: the draws bind the scene's
// depth pair and test against it (GEQUAL under reversed-Z) but never write
// it. The pass reprojects a pixel by its depth, a pixel with none falls on
// the far-plane path, and the far plane follows head ROTATION but not head
// TRANSLATION -- so a panel a metre away misregisters by pixels under an
// ordinary head sweep, NVIDIA's history rejects it there, and the raw
// jittered frame shows through as a half-pixel shuttle. That is the
// flicker; a partial rejection is the swim.
//
// THE FIX, and why it is this small
//
// For exactly those draws, swap the depth-stencil state for one that WRITES
// depth, keeping the game's own test (so the cockpit still occludes a
// panel). The depth then lands in the scene pair the pass, the depth probe
// and NVIDIA's depth copy already read, at the panel's true distance. No
// new targets, no eye pairing, no copy, no pass change. Three censuses said
// nothing downstream reads that depth after the UI draws except EDVR's own
// motion-vector dispatch (gate G10, docs/crisp-ui-handoff.md), and the
// panels' and the HUD's pixel shaders discard on alpha, so the depth lands
// on the strokes and the backing, not on empty quad. Flown 2026-09-06 on
// the Pimax under DLSS: "that looks much better".
//
// WHERE IT WRITES, and where it deliberately does not
//
// Only into the depth the pass reads. A draw whose bound depth target is
// not the scene pair (depthProbeIsSceneDepth) is left alone and counted:
// the main menu's panel binds a depth of its own that nothing reads, and
// writing there would be work for nothing (measured, review of
// 2026-09-06). And only while the pass is on: with fix.temporal_aa off the
// module does nothing at all, so the default costs a stock install nothing.
//
// WHAT COUNTS AS THE INTERFACE, without a list of composites
//
// Elite's GUI renderer is three shader families -- the textureless vector
// widget shader, the glyph-atlas text shader, the BC7 icon shader -- and
// every 2D panel is rasterised by them into an offscreen INTERFACE SURFACE
// that a mesh in the eye then samples. So: any target those families draw
// into is a UI surface (learned at the offscreen draws, kept for the
// session, validated by size and format when sampled so a recycled address
// cannot lie), and any eye draw whose pixel stage samples a UI surface is a
// UI composite. That one rule finds the 24 holo-panel draws without naming
// them. The flight HUD is vector geometry drawn straight into the eye and
// is named by its vertex shader's hash; advanced.ui_depth_families adds
// others, advanced.ui_depth_exclude removes any.
//
// The target direction indicator quad (vs 5DA53D8B0133341E) is left alone
// because it samples an authored atlas and lookup tables, never a learned
// surface -- not because the code refuses its target. It is drawn into the
// G-buffer before the lighting resolve, and a depth written there would
// light and fog the space behind the hologram; the log names every newly
// classified family with its target's size and format, so a family that
// ever lands somewhere new is visible in a field log.
//
// WHAT THE KEPT TEST COSTS
//
// With the game's test kept and depth now written, a piece of interface
// drawn LATER and FARTHER loses its overlap against an earlier, nearer one
// (the flight HUD is drawn before the panels). Nothing was seen on the
// first flight; advanced.ui_depth_test = always is the A/B if it ever is.
// The twin preserves the game's stencil settings, not the stencil OUTCOME
// of a depth test that now fails where it used to pass -- confined to the
// frame, since the pair is cleared with its stencil every frame before the
// interface draws.
//
// Free when off or when the pass is off: one bool on the draw path. On:
// two hashed memos (sampled views, vertex shaders) keep the per-draw work
// to pointer probes, with a handful of resolves a frame.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads fix.ui_depth (on | off), fix.temporal_aa (the gate),
// advanced.ui_depth_families, advanced.ui_depth_exclude and
// advanced.ui_depth_test. Install and reload; all live.
void uiDepthConfigure(Config& cfg);

// True while the key is on, the pass is on and nothing stood down: the
// draw path's one bool.
bool uiDepthWantsDraws();

// Every draw that did NOT land in an eye texture: learn the target as a UI
// surface when the bound vertex shader is one of the GUI renderer's
// families. The hash is asked only while a target is new, and a target
// checked to exhaustion is not asked again for a while.
void uiDepthNoteOffscreenDraw(ID3D11DeviceContext* ctx);

// Every eye draw: a UI composite (samples a learned surface in a
// pixel-stage slot 0..3) or a named direct family, with a depth target
// bound that is the scene pair's. True means the draw should write its
// depth; the caller wraps it in Begin/End.
bool uiDepthOnEyeDraw(ID3D11DeviceContext* ctx);

// Around the real draw: the depth-stencil state swapped for a writing twin
// of the game's own (derived once per game state and cached), and put back
// after -- the restore never skipped. A begin that cannot derive a twin
// leaves the draw untouched.
void uiDepthBegin(ID3D11DeviceContext* ctx);
void uiDepthEnd(ID3D11DeviceContext* ctx);

// Once per frame: the engage line, the totals line every 20 s.
void uiDepthFrameBoundary();

void uiDepthShutdown();

}  // namespace edvr
