// fix.ui_quality -- the engine-side panel sizing (docs/ui-layer-2026-09-23.md,
// "The engine's panel sizing"; docs/ui-sizing-owner-2026-09-23.md sections
// 3 and 5; the arithmetic and the build-332841 bytes in ui_sizing_math.h).
//
// Every Scaleform render-to-texture panel -- the cockpit's panels, the
// menus, station services, the menu's 16:9 screen -- is sized by the game as
// stage x W_ui x k / 1920 (/ 1080 for a view wider than 16:9), inlined in
// the panel init FUN_144570500 and the view-change recompute FUN_144571180.
// The four DIVSS operands of those divisions are pointed at two EDVR floats
// in a page within rel32 of the image, holding 1920 x f and 1080 x f with
// f = (W_ui x k) / (W_out x k_out) / T (clamped to [1/4, 1]), so the game
// itself makes, lays out, viewports and depth-partners every panel at its
// untrimmed size at HMD Quality = the key's target. The scene, the eye
// targets, the UI screen record and its 28 readers are not touched.
//
// SUPERSAMPLING AND THE SIZE BUDGET (2026-10-07). The game's c is the UI
// screen record's width times k, and the record's +0x30 is its +0x40 times
// Elite's Supersampling, so at Supersampling S > 1 every panel the game makes is
// S times wider: f carries the same S (the game is already denser by S), and
// the factor is raised, if need be, until the widest panel the formula could
// ask for (S x B / f, B the base c over the record's states) is within 7/8 of
// D3D11's 16384. A refused create is fatal in Elite, so it must never be this
// patch's doing: a launch at Supersampling 2.0 and ui_quality 125 asked for a
// 19200x10800 panel and died. ui_sizing_math.h holds the arithmetic; the
// Supersampling is read with HMD Quality (ui_surfaces.cpp) and without it there
// is no factor.
//
// LIVE SUPERSAMPLING AND THE NET (2026-10-08). The Supersampling is read from the game's own render context every
// frame (ctx+0x3564, found through two virtual slots EDVR's thunks remember the context from), and the .fxcfg's
// value, read every 5 s, is the fallback with the reason said. The setter's thunk moves the factor to the new value
// BEFORE the game's setter runs, so the panels its reconfigure recreates are made at the new factor. And behind all
// of it, a last-resort net at the panel's CreateTexture2D (ui_surfaces.cpp): a render or depth target over D3D11's
// 16384 a side is created shrunk to fit, aspect kept, never refused.
//
// SAFETY. Build-keyed: the PE stamp and image size, both sites' 30 bytes,
// the two functions' prologues and the three constants they read must be
// build 332841's exactly, or nothing is written and one line says which
// check failed. The operands are swapped once, the first time the key is on
// (all four or none, VirtualProtect and FlushInstructionCache as
// vscreen_res.cpp does); after that only the floats change -- aligned data
// stores -- and only when the factor's inputs settle on a new value, never
// per frame (the recompute runs twice per view change; a float changed
// between the two would part a viewport from its target). Key off: the
// floats go back to 1080 and 1920, the game's own values. Unload: the
// original operands are written back. The float page is never freed.
//
// THE CURSOR WINDOW. The one side reader of a panel's (w, h), FUN_14453EF80
// (panel vtbl +0x198), divides the panel's own size (+0x3B8/+0x3C0, which
// FUN_144571180 writes from these divisions) by the UI screen's (the
// renderer record's width and height, which this does not scale): a
// mouse-driven render-to-texture panel's cursor window grows by 1/f, up to
// the whole screen, and the cursor moves across the panel more slowly for
// the same mouse motion. Panels driven by focus (the cockpit's) are not
// affected.
#pragma once

#include <cstddef>

namespace edvr {

// From uiLayerConfigure: the key's target (0 off, 1.0, 1.25). The first time
// it is on, the sites are checked and the operands swapped (the floats at
// the game's own values until the factor's inputs are known).
void uiPanelScaleSetTarget(float target);

// Once a frame, on the render thread (uiLayerFrameBoundary): the factor from
// its inputs, written when they have settled on a new value.
void uiPanelScaleFrameBoundary();

// The engine is sizing the panels: the operands swapped, the key on and a
// factor written. Standing down (a game build these bytes are not), the
// panels stay at the game's own size until the patch is re-keyed. Lock-free.
bool uiPanelScaleLive();

// The factor the floats hold (1 when not live).
double uiPanelScaleFactor();

// The factor before the size budget (1 when not live): formula x Supersampling, clamped to [1/4, 1]. The orbit
// lines' width reads this one: it is the render/layer density ratio, which the budget (it thins the panels, never
// the layer) does not change. Equal to uiPanelScaleFactor() except where the budget raised that.
double uiPanelScaleLineFactor();

// max(Elite's Supersampling, 1) as the written factor carries it (1 when not live): the sizing chain's implied
// stage divides it out, the game's own panel width having multiplied by it.
double uiPanelScaleSupersampling();

// The Supersampling the factor was last made from, as chosen (live from the game's render context, or the .fxcfg's),
// and where it came from ("live", ".fxcfg" or "none"). 0 until the first choice. Lock-free.
double uiPanelScaleChosenSupersampling(const char** source);

// The operands written back (DLL unload). Idempotent.
void uiPanelScaleShutdown();

// The 30-second line (from ui_layer's totals).
void uiPanelScaleLog();

}  // namespace edvr
