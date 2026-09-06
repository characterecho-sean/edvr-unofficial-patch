// The UI writes its depth, so the temporal pass can follow it.
//
// THE DEFECT (Sean, 2026-09-05; measured on four censuses 2026-09-06)
//
// Under fix.temporal_aa every piece of Elite's 2D UI flickers and swims:
// the cockpit's holo panels, the flight HUD, the main menu's panel, the
// loading screen's text. Elite draws all of it depth-TESTED and depth-
// SILENT: the draws bind the scene's depth pair and test against it
// (GEQUAL under reversed-Z in the cockpit; test off in the menus) but never
// write it. The pass reprojects a pixel by its depth, a pixel with none
// falls on the far-plane path, and the far plane follows head ROTATION but
// not head TRANSLATION -- so a panel a metre away misregisters by pixels
// under an ordinary head sweep, NVIDIA's history rejects it there, and the
// raw jittered frame shows through as a half-pixel shuttle. That is the
// flicker; a partial rejection is the swim.
//
// THE FIX, and why it is this small
//
// For exactly those draws, swap the depth-stencil state for one that WRITES
// depth, keeping the game's own test where it had one (so the cockpit still
// occludes a panel) and testing ALWAYS where it had none (the menus). The
// depth then lands in the scene pair the pass, the depth probe and NVIDIA's
// depth copy already read, at the panel's true distance. No new targets, no
// eye pairing, no copy, no pass change. The census said nothing downstream
// reads that depth after the UI draws except EDVR's own pass (gate G10,
// docs/crisp-ui-handoff.md), and the UI's pixel shaders discard on alpha,
// so the depth lands on the strokes and the backing, not on empty quad.
//
// WHAT COUNTS AS UI, without a list of composites
//
// Elite's GUI renderer is three shader families -- the textureless vector
// widget shader, the glyph-atlas text shader, the BC7 icon shader -- and
// every 2D panel is rasterised by them into an offscreen INTERFACE SURFACE
// that a mesh in the eye then samples. So: any target those families draw
// into is a UI surface (learned at the offscreen draws, kept for the
// session), and any eye draw whose pixel stage samples a UI surface is a UI
// composite. That one rule finds the 24 holo-panel draws, the menu's panel
// and the loader's 5760-index composite without naming any of them. The
// flight HUD is vector geometry drawn straight into the eye and is named
// by its vertex shader's hash; advanced.ui_depth_families adds others.
//
// WHAT IS LEFT ALONE, on purpose
//
// The target indicator quad (vs 5DA53D8B0133341E) is drawn into the
// G-buffer stage, before the lighting resolve reads the depth for every
// pixel; a depth written there would light and fog the space behind the
// hologram as if it were a surface. It stays depthless. So does anything
// whose target is not the lit HDR buffer or the tonemapped 8-bit one --
// the log names each family's target, so a new case is visible.
//
// Off by default until flown. Free when off: one bool on the draw path.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads fix.ui_depth (off | on), advanced.ui_depth_families and
// advanced.ui_depth_exclude. Install and reload; live.
void uiDepthConfigure(Config& cfg);

// True while on and not stood down: the draw path's one bool.
bool uiDepthWantsDraws();

// Every draw that did NOT land in an eye texture: learn the target as a UI
// surface when the bound vertex shader is one of the GUI renderer's
// families. Cheap: the hash is asked only until a target is known.
void uiDepthNoteOffscreenDraw(ID3D11DeviceContext* ctx);

// Every eye draw: is this a UI composite (samples a learned surface in a
// pixel-stage slot 0..3) or a named direct family? True means the draw
// should write its depth; the caller wraps it in Begin/End.
bool uiDepthOnEyeDraw(ID3D11DeviceContext* ctx);

// Around the real draw: the depth-stencil state swapped for a writing twin
// of the game's own (derived once per game state and cached), and put back
// after. A begin that cannot derive one leaves the draw untouched.
void uiDepthBegin(ID3D11DeviceContext* ctx);
void uiDepthEnd(ID3D11DeviceContext* ctx);

// Once per frame: the engage line, the totals line every 20 s.
void uiDepthFrameBoundary();

void uiDepthShutdown();

}  // namespace edvr
